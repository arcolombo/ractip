/*
 * $Id$
 * 
 * Copyright (C) 2010 Kengo Sato
 *
 * This file is part of RactIP.
 *
 * RactIP is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RactIP is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RactIP.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "cmdline.h"
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <list>
#include "fa.h"
#include "ip.h"

#include "contrafold/SStruct.hpp"
#include "contrafold/InferenceEngine.hpp"
#include "contrafold/DuplexEngine.hpp"
#include "contrafold/Defaults.ipp"

namespace Vienna {
extern "C" {
#include <ViennaRNA/fold.h>
#include <ViennaRNA/fold_vars.h>
#include <ViennaRNA/part_func.h>
#include <ViennaRNA/utils.h>
#include "pf_duplex.h"
  extern void read_parameter_file(const char fname[]);
};
};

extern "C" {
#include "boltzmann_param.h"
};

typedef unsigned int uint;
typedef std::vector<float> VF;
typedef std::vector<VF> VVF;
typedef std::vector<int> VI;
typedef std::vector<VI> VVI;

class RactIP
{
public:
  RactIP()
    : th_hy_(0.2),
      th_ss_(0.5),
      alpha_(0.5),
      in_pk_(true),
      use_contrafold_(true),
      stacking_constraints_(true),
      show_energy_(false),
      n_th_(1),
      rip_file_(),
      param_file_(),
      fa1_(),
      fa2_()
  {
  }
  
  ~RactIP()
  {
  }

  RactIP& parse_options(int& argc, char**& argv);
  int run();
  void solve(const std::string& s1, const std::string& s2,
             std::string& r1, std::string& r2);

private:
  void contrafold(const std::string& seq, VF& bp, VI& offset) const;
  void contraduplex(const std::string& seq1, const std::string& seq2, VVF& hp) const;
  void rnafold(const std::string& seq, VF& bp, VI& offset) const;
  void rnaduplex(const std::string& seq1, const std::string& seq2, VVF& hp) const;
  void load_from_rip(const char* filename,
                     const std::string& s1, const std::string& s2,
                     VF& bp1, VI& offset1, VF& bp2, VI& offset2, VVF& hp) const;

private:
  // options
  float th_hy_;                // threshold for the hybridization probability
  float th_ss_;                // threshold for the base-pairing probability
  float alpha_;                // weight for the hybridization score
  bool in_pk_;                 // allow internal pseudoknots or not
  bool use_contrafold_;        // use CONTRAfold model or not
  bool stacking_constraints_;
  bool show_energy_;
  int n_th_;                   // the number of threads
  std::string rip_file_;
  std::string param_file_;
  std::string fa1_;
  std::string fa2_;
};

void
RactIP::
contrafold(const std::string& seq, VF& bp, VI& offset) const
{
  SStruct ss("unknown", seq);
  ParameterManager<float> pm;
  InferenceEngine<float> en(false);
  VF w = GetDefaultComplementaryValues<float>();
  bp.resize((seq.size()+1)*(seq.size()+2)/2, 0.0);
  en.RegisterParameters(pm);
  en.LoadValues(w);
  en.LoadSequence(ss);
  en.ComputeInside();
  en.ComputeOutside();
  en.ComputePosterior();
  en.GetPosterior(0, bp, offset);
}    

void
RactIP::
contraduplex(const std::string& seq1, const std::string& seq2, VVF& hp) const
{
  hp.resize(seq1.size()+1, VF(seq2.size()+1));
  SStruct ss1("unknown", seq1), ss2("unknown", seq2);
  ParameterManager<float> pm;
  DuplexEngine<float> en(false);
  VF w = GetDefaultComplementaryValues<float>();
  VF ip;
  en.RegisterParameters(pm);
  en.LoadValues(w);
  en.LoadSequence(ss1, ss2);
  en.ComputeInside();
  en.ComputeOutside();
  en.ComputePosterior();
  en.GetPosterior(th_hy_, ip);
  for (uint i=0; i!=seq1.size(); ++i)
    for (uint j=0; j!=seq2.size(); ++j)
      hp[i+1][j+1] = ip[en.GetOffset(i+1)+(j+1)];
}

void
RactIP::
rnafold(const std::string& seq, VF& bp, VI& offset) const
{
  uint L=seq.size();
  bp.resize((L+1)*(L+2)/2);
  offset.resize(L+1);
  for (uint i=0; i<=L; ++i)
    offset[i] = i*((L+1)+(L+1)-i-1)/2;
#if 0
  std::string str(seq.size()+1, '.');
  float min_en = Vienna::fold(const_cast<char*>(seq.c_str()), &str[0]);
  float sfact = 1.07;
  float kT = (Vienna::temperature+273.15)*1.98717/1000.; /* in Kcal */
  Vienna::pf_scale = exp(-(sfact*min_en)/kT/seq.size());
#else
  Vienna::pf_scale = -1;
#endif
#ifndef HAVE_VIENNA20
  Vienna::init_pf_fold(L);
#endif
  Vienna::pf_fold(const_cast<char*>(seq.c_str()), NULL);
#ifdef HAVE_VIENNA20
  FLT_OR_DBL* pr = Vienna::export_bppm();
  int* iindx = Vienna::get_iindx(seq.size());
#else
  FLT_OR_DBL* pr = Vienna::pr;
  int* iindx = Vienna::iindx;
#endif
  for (uint i=0; i!=L-1; ++i)
    for (uint j=i+1; j!=L; ++j)
      bp[offset[i+1]+(j+1)] = pr[iindx[i+1]-(j+1)];
  Vienna::free_pf_arrays();
}

void
RactIP::
rnaduplex(const std::string& s1, const std::string& s2, VVF& hp) const
{
  hp.resize(s1.size()+1, VF(s2.size()+1));
  Vienna::pf_duplex(s1.c_str(), s2.c_str());
  for (uint i=0; i!=s1.size(); ++i)
    for (uint j=0; j!=s2.size(); ++j)
      hp[i+1][j+1] = Vienna::pr_duplex[i+1][j+1];
  Vienna::free_pf_duplex();
}

void
RactIP::
load_from_rip(const char* filename,
              const std::string& s1, const std::string& s2,
              VF& bp1, VI& offset1, VF& bp2, VI& offset2, VVF& hp) const
{
  enum { NONE, TABLE_R, TABLE_S, TABLE_I };
  uint st=NONE;

  uint L1=s1.size();
  bp1.resize((L1+1)*(L1+2)/2);
  offset1.resize(L1+1);
  for (uint i=0; i<=L1; ++i)
    offset1[i] = i*((L1+1)+(L1+1)-i-1)/2;

  uint L2=s2.size();
  bp2.resize((L2+1)*(L2+2)/2);
  offset2.resize(L2+1);
  for (uint i=0; i<=L2; ++i)
    offset2[i] = i*((L2+1)+(L2+1)-i-1)/2;

  hp.resize(L1+1, VF(L2+1));

  std::ifstream is(filename);
  std::string l;
  while (std::getline(is, l))
  {
    if (strncmp(l.c_str(), "Table R:", 8)==0) st=TABLE_R;
    else if (strncmp(l.c_str(), "Table S:", 8)==0) st=TABLE_S;
    else if (strncmp(l.c_str(), "Table I:", 8)==0) st=TABLE_I;
    else if (st!=NONE && l[0]>='0' && l[0]<='9')
    {
      int i,j;
      double p;
      std::istringstream s(l.c_str());
      s >> i >> j >> p;
      switch (st)
      {
        case TABLE_R:
          bp1[offset1[i]+j] = p;
          break;
        case TABLE_S:
          bp2[offset2[L2-j+1]+L2-i+1] = p;
          break;
        case TABLE_I:
          hp[i][L2-j+1] = p;
          break;
        default:
          break;
      }
    }
    else st=NONE;
  }
}

void
RactIP::
solve(const std::string& s1, const std::string& s2, std::string& r1, std::string& r2)
{
  IP ip(IP::MAX, n_th_);
  VF bp1, bp2;
  VI offset1, offset2;
  VVF hp;

  // calculate posterior probability matrices
  if (!rip_file_.empty())
  {
    load_from_rip(rip_file_.c_str(), s1, s2, bp1, offset1, bp2, offset2, hp);
  }
  else if (use_contrafold_)
  {
    contrafold(s1, bp1, offset1);
    contrafold(s2, bp2, offset2);
    //contraduplex(s1, s2, hp);
    rnaduplex(s1, s2, hp);
  }
  else
  {
    Vienna::pf_scale = -1;
    rnafold(s1, bp1, offset1);
    rnafold(s2, bp2, offset2);
    rnaduplex(s1, s2, hp);
  }
  
  // make objective variables with their weights
  VVI x(s1.size(), VI(s1.size(), -1));
  VVI xx(s1.size());
  for (uint j=1; j!=s1.size(); ++j)
  {
    for (uint i=j-1; i!=-1u; --i)
    {
      const float& p=bp1[offset1[i+1]+(j+1)];
      if (p>th_ss_)
      {
        x[i][j] = ip.make_variable(p);
        xx[i].push_back(j);
      }
    }
  }

  VVI y(s2.size(), VI(s2.size(), -1));
  VVI yy(s2.size());
  for (uint j=1; j!=s2.size(); ++j)
  {
    for (uint i=j-1; i!=-1u; --i)
    {
      const float& p=bp2[offset2[i+1]+(j+1)];
      if (p>th_ss_)
      {
        y[i][j] = ip.make_variable(p);
        yy[i].push_back(j);
      }
    }
  }

  VVI z(s1.size(), VI(s2.size(), -1));
  std::vector< std::vector<int> > zz(s1.size());
  for (uint i=0; i!=s1.size(); ++i)
  {
    for (uint j=0; j!=s2.size(); ++j)
    {
      const float& p=hp[i+1][j+1];
      if (p>th_hy_)
      {
        z[i][j] = ip.make_variable(alpha_*p);
        zz[i].push_back(j);
      }
    }
  }
  ip.update();

  // constraint 1: each a_i is paired with at most one base
  for (uint i=0; i!=s1.size(); ++i)
  {
    int row = ip.make_constraint(IP::UP, 0, 1);
    for (uint j=0; j!=s2.size(); ++j)
      if (z[i][j]>=0)
        ip.add_constraint(row, z[i][j], 1);
    for (uint j=0; j<i; ++j)
      if (x[j][i]>=0)
        ip.add_constraint(row, x[j][i], 1);
    for (uint j=i+1; j<s1.size(); ++j)
      if (x[i][j]>=0)
        ip.add_constraint(row, x[i][j], 1);
  }

  // constraint 2: each b_i is paired with at moat one
  for (uint i=0; i!=s2.size(); ++i)
  {
    int row = ip.make_constraint(IP::UP, 0, 1);
    for (uint j=0; j!=s1.size(); ++j)
      if (z[j][i]>=0)
        ip.add_constraint(row, z[j][i], 1);
    for (uint j=0; j<i; ++j)
      if (y[j][i]>=0)
        ip.add_constraint(row, y[j][i], 1);
    for (uint j=i+1; j<s2.size(); ++j)
      if (y[i][j]>=0)
        ip.add_constraint(row, y[i][j], 1);
  }

  // constraint 3: disallow external pseudoknots
  for (uint i=0; i<zz.size(); ++i)
  {
    for (uint k=i+1; k<zz.size(); ++k)
    {
      for (uint p=0; p<zz[i].size(); ++p)
      { 
        uint j=zz[i][p];
        for (uint q=0; q<zz[k].size(); ++q)
        {
          uint l=zz[k][q];
          if (j<l)
          {
            int row = ip.make_constraint(IP::UP, 0, 1);
            ip.add_constraint(row, z[i][j], 1);
            ip.add_constraint(row, z[k][l], 1);
          }
        }
      }
    }
  }

  if (in_pk_)
  {
    // constraint 4: disallow internal pseudoknots in a
    for (uint i=0; i<xx.size(); ++i)
    {
      for (uint p=0; p<xx[i].size(); ++p)
      {
        uint j=xx[i][p];
        for (uint k=i+1; k<j; ++k)
        {
          for (uint q=0; q<xx[k].size(); ++q)
          {
            uint l=xx[k][q];
            if (j<l)
            {
              int row = ip.make_constraint(IP::UP, 0, 1);
              ip.add_constraint(row, x[i][j], 1);
              ip.add_constraint(row, x[k][l], 1);
            }
          }
        }
      }
    }

    // constraint 5: disallow internal pseudoknots in b
    for (uint i=0; i<yy.size(); ++i)
    {
      for (uint p=0; p<yy[i].size(); ++p)
      {
        uint j=yy[i][p];
        for (uint k=i+1; k<j; ++k)
        {
          for (uint q=0; q<yy[k].size(); ++q)
          {
            uint l=yy[k][q];
            if (j<l)
            {
              int row = ip.make_constraint(IP::UP, 0, 1);
              ip.add_constraint(row, y[i][j], 1);
              ip.add_constraint(row, y[k][l], 1);
            }
          }
        }
      }
    }
  }

  if (stacking_constraints_)
  {
    // upstream of s1
    for (uint i=0; i<s1.size(); ++i)
    {
      int row = ip.make_constraint(IP::LO, 0, 0);
      for (uint j=0; j<i; ++j)
        if (x[j][i]>=0)
          ip.add_constraint(row, x[j][i], -1);
      if (i>0)
        for (uint j=0; j<i-1; ++j)
          if (x[j][i-1]>=0)
            ip.add_constraint(row, x[j][i-1], 1);
      if (i+1<s1.size())
        for (uint j=0; j<i+1; ++j)
          if (x[j][i+1]>=0)
            ip.add_constraint(row, x[j][i+1], 1);
    }

    // downstream of s1
    for (uint i=0; i<s1.size(); ++i)
    {
      int row = ip.make_constraint(IP::LO, 0, 0);
      for (uint j=i+1; j<s1.size(); ++j)
        if (x[i][j]>=0)
          ip.add_constraint(row, x[i][j], -1);
      if (i>0)
        for (uint j=i; j<s1.size(); ++j)
          if (x[i-1][j]>=0)
            ip.add_constraint(row, x[i-1][j], 1);
      if (i+1<s1.size())
        for (uint j=i+2; j<s1.size(); ++j)
          if (x[i+1][j]>=0)
            ip.add_constraint(row, x[i+1][j], 1);
    }

    // upstream of s2
    for (uint i=0; i<s2.size(); ++i)
    {
      int row = ip.make_constraint(IP::LO, 0, 0);
      for (uint j=0; j<i; ++j)
        if (y[j][i]>=0)
          ip.add_constraint(row, y[j][i], -1);
      if (i>0)
        for (uint j=0; j<i-1; ++j)
          if (y[j][i-1]>=0)
            ip.add_constraint(row, y[j][i-1], 1);
      if (i+1<s2.size())
        for (uint j=0; j<i+1; ++j)
          if (y[j][i+1]>=0)
            ip.add_constraint(row, y[j][i+1], 1);
    }

    // downstream of s1
    for (uint i=0; i<s2.size(); ++i)
    {
      int row = ip.make_constraint(IP::LO, 0, 0);
      for (uint j=i+1; j<s2.size(); ++j)
        if (y[i][j]>=0)
          ip.add_constraint(row, y[i][j], -1);
      if (i>0)
        for (uint j=i; j<s2.size(); ++j)
          if (y[i-1][j]>=0)
            ip.add_constraint(row, y[i-1][j], 1);
      if (i+1<s2.size())
        for (uint j=i+2; j<s2.size(); ++j)
          if (y[i+1][j]>=0)
            ip.add_constraint(row, y[i+1][j], 1);
    }

    // for s2
    for (uint i=0; i<s2.size(); ++i)
    {
      int row = ip.make_constraint(IP::LO, 0, 0);
      for (uint j=0; j<s1.size(); ++j)
        if (z[j][i]>=0)
          ip.add_constraint(row, z[j][i], -1);
      if (i>0)
        for (uint j=0; j<s1.size(); ++j)
          if (z[j][i-1]>=0)
            ip.add_constraint(row, z[j][i-1], 1);
      if (i+1<s2.size())
        for (uint j=0; j<s1.size(); ++j)
          if (z[j][i+1]>=0)
            ip.add_constraint(row, z[j][i+1], 1);
    }

    // for s1
    for (uint i=0; i<s1.size(); ++i)
    {
      int row = ip.make_constraint(IP::LO, 0, 0);
      for (uint j=0; j<s2.size(); ++j)
        if (z[i][j]>=0)
          ip.add_constraint(row, z[i][j], -1);
      if (i>0)
        for (uint j=0; j<s2.size(); ++j)
          if (z[i-1][j]>=0)
            ip.add_constraint(row, z[i-1][j], 1);
      if (i+1<s1.size())
        for (uint j=0; j<s2.size(); ++j)
          if (z[i+1][j]>=0)
            ip.add_constraint(row, z[i+1][j], 1);
    }
  }

  // execute optimization
  ip.solve();

  // build the resultant structure
  r1.resize(s1.size());
  r2.resize(s2.size());
  std::fill(r1.begin(), r1.end(), '.');
  std::fill(r2.begin(), r2.end(), '.');
  for (uint i=0; i!=s1.size(); ++i)
  {
    for (uint j=0; j!=s2.size(); ++j)
    {
      if (z[i][j]>=0 && ip.get_value(z[i][j])>0.5)
      {
        r1[i]='['; r2[j]=']';
      }
    }
  }
  if (in_pk_)
  {
    for (uint i=0; i<s1.size(); ++i)
    {
      for (uint j=i+1; j<s1.size(); ++j)
      {
        if (x[i][j]>=0 && ip.get_value(x[i][j])>0.5)
        {
          assert(r1[i]=='.'); assert(r1[j]=='.');
          r1[i]='('; r1[j]=')';
        }
      }
    }
    for (uint i=0; i<s2.size(); ++i)
    {
      for (uint j=i+1; j<s2.size(); ++j)
      {
        if (y[i][j]>=0 && ip.get_value(y[i][j])>0.5)
        {
          assert(r2[i]=='.'); assert(r2[j]=='.');
          r2[i]='('; r2[j]=')';
        }
      }
    }
  }
}

RactIP&
RactIP::
parse_options(int& argc, char**& argv)
{
  gengetopt_args_info args_info;
  if (cmdline_parser(argc, argv, &args_info)!=0) exit(1);

  alpha_ = args_info.alpha_arg;
  th_ss_ = args_info.fold_th_arg;
  th_hy_ = args_info.hybridize_th_arg;
  in_pk_ = args_info.no_pk_flag==0;
  use_contrafold_ = args_info.mccaskill_flag==0;
  stacking_constraints_ = args_info.allow_isolated_flag==0;
  n_th_ = 1; // args_info.n_th_arg;
  if (args_info.rip_given) rip_file_ = args_info.rip_arg;
  show_energy_ = args_info.show_energy_flag==1;
  if (args_info.param_file_given) param_file_ = args_info.param_file_arg;

  if (args_info.inputs_num==0)
  {
    cmdline_parser_print_help();
    cmdline_parser_free(&args_info);
    exit(1);
  }
  if (args_info.inputs_num>=1)
    fa1_ = args_info.inputs[0];
  if (args_info.inputs_num>=2)
    fa2_ = args_info.inputs[1];

  cmdline_parser_free(&args_info);
  return *this;
}

int
RactIP::
run()
{
  // set the energy parameters
  copy_boltzmann_parameters();
  if (!param_file_.empty())
    Vienna::read_parameter_file(param_file_.c_str());
  
  // read sequences
  Fasta fa1, fa2;
  if (!fa1_.empty() && !fa2_.empty())
  {
    std::list<Fasta> l1, l2;
    if (Fasta::load(l1, fa1_.c_str())==0)
      throw (fa1_+": Format error").c_str();
    if (Fasta::load(l2, fa2_.c_str())==0)
      throw (fa2_+": Format error").c_str();
    fa1=l1.front();
    fa2=l2.front();
  }
  else if (!fa1_.empty())
  {
    std::list<Fasta> l1;
    if (Fasta::load(l1, fa1_.c_str())<2)
      throw (fa1_+": Format error").c_str();
    std::list<Fasta>::const_iterator x=l1.begin();
    fa1=*(x++);
    fa2=*(x++);
  }
  else { throw "unreachable"; }

  // predict the interation
  std::string r1, r2;
  solve(fa1.seq(), fa2.seq(), r1, r2);

  // display the result
  std::cout << ">" << fa1.name() << std::endl
            << fa1.seq() << std::endl << r1 << std::endl
            << ">" << fa2.name() << std::endl
            << fa2.seq() << std::endl << r2 << std::endl;

  // show energy of the joint structure
  if (show_energy_)
  {
#ifdef HAVE_VIENNA20
    float e1=Vienna::energy_of_structure(fa1.seq().c_str(), r1.c_str(), -1);
    float e2=Vienna::energy_of_structure(fa2.seq().c_str(), r2.c_str(), -1);
#else
    Vienna::eos_debug = -1;
    float e1=Vienna::energy_of_struct(fa1.seq().c_str(), r1.c_str());
    float e2=Vienna::energy_of_struct(fa2.seq().c_str(), r2.c_str());
#endif
    std::string ss(fa1.seq()+"NNN"+fa2.seq());

    std::string r1_temp(r1);
    for (std::string::iterator x=r1_temp.begin(); x!=r1_temp.end(); ++x)
    {
      switch (*x)
      {
        case '(': case ')': *x='.'; break;
        case '[': *x='('; break;
        default: break;
      }
    }

    std::string r2_temp(r2);
    for (std::string::iterator x=r2_temp.begin(); x!=r2_temp.end(); ++x)
    {
      switch (*x)
      {
        case '(': case ')': *x='.'; break;
        case ']': *x=')'; break;
        default: break;
      }
    }

    std::string rr(r1_temp+"..."+r2_temp);
    //std::cout << ss << std::endl << rr << std::endl;
#ifdef HAVE_VIENNA20
    float e3=Vienna::energy_of_structure(ss.c_str(), rr.c_str(), -1);
#else
    Vienna::eos_debug = -1;
    float e3=Vienna::energy_of_struct(ss.c_str(), rr.c_str());
#endif

    std::cout << "(E: S1=" << e1 << ", "
              << "S2=" << e2 << ", "
              << "H=" << e3 << ", "
              << "JS=" << e1+e2+e3 << ")" << std::endl;
  }

  return 0;
}

int
main(int argc, char* argv[])
{
  try
  {
    RactIP ractip;
    return ractip.parse_options(argc, argv).run();
  }
  catch (const char* msg)
  {
    std::cout << msg << std::endl;
  }
  catch (std::logic_error err)
  {
    std::cout << err.what() << std::endl;
  }
  return 1;
}
