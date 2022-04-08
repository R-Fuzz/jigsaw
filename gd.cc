#include <stdint.h>
#include <assert.h>
#include <iostream>
#include "input.h"
#include "grad.h"
#include "config.h"
#include "test.h"
#include "rgd_op.h"

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
void dumpResults(MutInput &input, struct FUT* fut) {
  int i = 0;
  for (auto it : fut->inputs) {
    std::cout << "index is " << it.first << " result is " << (int)input.value[i] << std::endl;
    i++;
  }
}
void addResults(MutInput &input, struct FUT* fut) {
  int i = 0;
  for (auto it : fut->inputs) {
    (*fut->rgd_solution)[it.first] = input.value[i];
    i++;
  }
}
void addOptiResults(MutInput &input, struct FUT* fut) {
  int i = 0;
  for (auto it : fut->inputs) {
    (*fut->opti_solution)[it.first] = input.value[i];
    i++;
  }
}


inline uint64_t sat_inc(uint64_t base, uint64_t inc) {
  return base+inc < base ? -1 : base+inc;
}

uint64_t getDistance(uint32_t comp, uint64_t a, uint64_t b) {
  uint64_t dis = 0;
  switch (comp) {
    case rgd::Equal:	
      if (a>=b) dis = a-b;
      else dis=b-a;
      break;
    case rgd::Distinct:	
      if (a==b) dis = 1;
      else dis = 0;
      break;
    case rgd::Ult:	
      if (a<b) dis = 0;
      else dis = sat_inc(a-b,1);
      break;
    case rgd::Ule:	
      if (a<=b) dis = 0;
      else dis = a-b;
      break;
    case rgd::Ugt:	
      if (a>b) dis = 0;
      else dis = sat_inc(b-a,1);
      break;
    case rgd::Uge:	
      if (a>=b) dis = 0;
      else dis = b-a;
      break;
    case rgd::Slt:	
      if ((int64_t)a < (int64_t)b) return 0;
      else dis = sat_inc(a-b,1);
      break;
    case rgd::Sle:	
      if ((int64_t)a <= (int64_t)b) return 0;
      else dis = a-b;
      break;
    case rgd::Sgt:	
      if ((int64_t)a > (int64_t)b) return 0;
      else dis = sat_inc(b-a,1);
      break;
    case rgd::Sge:	
      if ((int64_t)a >= (int64_t)b) return 0;
      else dis = b-a;
      break;
    default:
      assert(0);
  }
  return dis;
}

void single_distance(MutInput &input, struct FUT* fut, int index) {
  uint64_t cur = 0;
  for(std::shared_ptr<Constraint> c : fut->cmap[index]) {
    int arg_idx = 0;	
    for (auto arg : c->input_args) {
      if (arg.first) {// symbolic
        fut->scratch_args[2+arg_idx] = (uint64_t)input.value[arg.second];
      }
      else {
        fut->scratch_args[2+arg_idx] = arg.second;
      }
      ++arg_idx;
    }
    cur = (uint64_t)c->fn(fut->scratch_args);
    uint64_t dis = getDistance(c->comparison,fut->scratch_args[0],fut->scratch_args[1]);
    fut->distances[c->index] = dis;
  }
}


uint64_t distance(MutInput &input, struct FUT* fut) {
  static int timeout = 0;
  static int solved= 0;
  uint64_t res = 0;
  uint64_t cur = 0;
  uint64_t dis0 = 0;

  //	for(int i=0; i<1; i++) {
  for(int i=0; i< fut->constraints.size(); i++) {
    //mapping symbolic args
    int arg_idx = 0;	
    std::shared_ptr<Constraint> c = fut->constraints[i];
    for (auto arg : c->input_args) {
      if (arg.first) {// symbolic
        fut->scratch_args[2+arg_idx] = (uint64_t)input.value[arg.second];
      }
      else {
        fut->scratch_args[2+arg_idx] = arg.second;
      }
      ++arg_idx;
    }
    //for(int p=0;p<fut->n_sym_args+fut->n_const_args;p++) std::cout << (int)fut->scratch_args[p]<<", ";
    //std::cout << std::endl;
    cur = (uint64_t)c->fn(fut->scratch_args);
    uint64_t dis = getDistance(c->comparison,fut->scratch_args[0],fut->scratch_args[1]);
    fut->distances[i] = dis;
    if(i==0) dis0 = dis;
    /*
       if (dis == 0 && i == 0 && !fut->opti_hit) {
       fut->opti_hit = true;
       addOptiResults(input, fut);
       }
     */
    //printf("func called and expr %d, comparison %d, arg0 %lu and arg1 %lu and return value is %lu \n",i, c.comparison, fut->scratch_args[0], fut->scratch_args[1], dis);
    //	if (cur>0)
    ///		res += cur;
    if (dis>0) {
      res = sat_inc(res,dis);
    }
  }
  if (res==0) {
    fut->stopped = true;
    fut->gsol = true;
    //dumpResults(input, fut);
    //fut->scratch_args[24] = fut->scratch_args[24] & 0x1f;
    addResults(input, fut);
  }
  fut->att++;
  if (fut->att>MAX_EXEC_TIMES) {
    fut->stopped = true;
    fut->gsol = false;
  }
  return res;
}



void partial_derivative(MutInput &orig_input, size_t index, uint64_t f0, bool *sign, bool* is_linear, uint64_t *val, struct FUT* fut) {
  //TODO assign constructors
  //MutInput input = orig_input;
  //std::cout << "calculating partial derivative and orig_input is " << orig_input.get(0) << " and " << orig_input.get(1) << std::endl;
  //std::cout << "calculating partial derivative and input is " << input.get(0) << " and " << input.get(1) << std::endl;
  //int idx = 0;
  //for(auto i : orig_input.value)
  //	fut->scratch_args[idx++] = i;

  uint8_t orig_val = orig_input.get(index);
  //	uint8_t orig_val = fut->scratch_args[index];
  orig_input.update(index,true,1);

  //	fut->scratch_args[index]++;
  //	uint64_t f_plus = execute(fut->scratch_args,fut);
  single_distance(orig_input,fut, index);
  uint64_t f_plus = 0;
  for (int i=0;i<fut->constraints.size();i++)
    f_plus = sat_inc(f_plus, fut->distances[i]);

  fut->att += 1;
  if (fut->att > MAX_EXEC_TIMES)
    fut->stopped = true;
  orig_input.set(index,orig_val);
  fut->distances = fut->orig_distances;
  //	fut->scratch_args[index] = orig_val;
  if (fut->stopped) { *val = 0; return;}
  //orig_input.set(index,orig_val);
  orig_input.update(index,false,1);

  //	fut->scratch_args[index]--;
  //	uint64_t f_minus = execute(fut->scratch_args,fut);
  uint64_t f_minus = 0;
  single_distance(orig_input,fut, index);

  for (int i=0;i<fut->constraints.size();i++)
    f_minus += fut->distances[i];
  fut->att += 1;
  if (fut->att > MAX_EXEC_TIMES)
    fut->stopped = true;
  //	fut->scratch_args[index] = orig_val;
  orig_input.set(index,orig_val);	

  if (fut->stopped) { *val = 0; return;}

  fut->distances = fut->orig_distances;
  //std::cout << "calculating partial and f0 is " << f0 << " f_minus is" << f_minus << " and f_plus is " << f_plus << std::endl;

  if (f_minus < f0) {
    if (f_plus < f0) {
      if (f_minus < f_plus) {
        *sign = false;
        *is_linear = false;
        *val = f0 - f_minus;
      } else {
        *sign = true;
        *is_linear = false;
        *val = f0 - f_plus;
      }
    } else {
      *sign = false;
      *is_linear = ((f_minus != f0) && (f0 - f_minus == f_plus -f0));
      *val = f0 -f_minus;
    }
  } else {
    if (f_plus < f0) {
      *sign = true;
      *is_linear = ((f_minus != f0) && (f_minus - f0 == f0 - f_plus));
      *val = f0 - f_plus;
    }
    else {
      *sign = true;
      *is_linear = false;
      *val = 0;
    }
  }
}

void compute_delta_all(MutInput &input, Grad &grad, size_t step) {
  double fstep = (double)step; 
  int index = 0;
  for(auto &gradu : grad.get_value()) {
    double movement = gradu.pct * step;
    input.update(index,gradu.sign,(uint64_t)movement);
    index++;
  } 
}



void cal_gradient(MutInput &input, uint64_t f0, Grad &grad, struct FUT *fut) {
  uint64_t max = 0;
  int index = 0;
  for(auto &gradu : grad.get_value()) {

    //std::cout << "cal_gradient" << std::endl;
    if (fut->stopped) {
      break;
    }
    bool sign = false;
    bool is_linear = false;
    uint64_t val = 0;
    partial_derivative(input,index,f0,&sign,&is_linear,&val,fut);
    if (val > max) {
      max = val;
    }
    //linear = linear && l;
    gradu.sign = sign;
    gradu.val = val;
    index++;
  }
}



uint64_t descend(MutInput &input_min, MutInput &input, uint64_t f0, Grad &grad, struct FUT* fut) {
  uint64_t f_last = f0;
  input = input_min;
  bool doDelta = false;
  int deltaIdx = 0;

  uint64_t vsum = grad.val_sum();

  if (vsum > 0) {
    auto guess_step = f0 / vsum;
    compute_delta_all(input,grad,guess_step);
    uint64_t f_new = distance(input,fut);
    if (f_new >= f_last) {
      input = input_min;
    } else {
      input_min = input;
      f_last = f_new;
    }
  }


  size_t step = 1;
  while (true) {
    while (true) {
      if (fut->stopped) {
        return f_last;
      }

      uint64_t f_new = 0;
      if (doDelta) {
        double movement = grad.get_value()[deltaIdx].pct * (double)step;
        input.update(deltaIdx,grad.get_value()[deltaIdx].sign,(uint64_t)movement);

        single_distance(input,fut, deltaIdx);
        for (int i=0;i<fut->constraints.size();i++)
          f_new += fut->distances[i];
        fut->att += 1;
        if (fut->att > MAX_EXEC_TIMES)
          fut->stopped = true;

      } else {
        compute_delta_all(input, grad, step);
        f_new = distance(input,fut);
      }


      if (f_new >= f_last) {
        //if (f_new == UINTMAX_MAX)
        break;
      }

      step *= 2;
      input_min = input;
      f_last = f_new;
    }
    //break;

    if (grad.len() == 1) {
      break;
    } else {
      if (doDelta) deltaIdx++; 
      else { deltaIdx = 0; doDelta = true;}
      while ((deltaIdx < grad.len()) && grad.get_value()[deltaIdx].pct < 0.01) {
        deltaIdx++ ;
      }
      if (deltaIdx >= grad.len()) {
        break;
      }
      input = input_min;
      step = 1;
    }
  }
  return f_last;
}


uint64_t repick_start_point(MutInput &input_min, struct FUT* fut) {
  input_min.randomize();
  uint64_t ret = distance(input_min,fut);
  fut->orig_distances = fut->distances;
  return ret;
}

uint64_t reload_input(MutInput &input_min,struct FUT* fut) {
  input_min.assign(fut->inputs);
#if 0
  printf("assign realod\n");
  for(auto itr : fut->inputs) {
    printf("offset %u value %u\n", itr.first,itr.second);
  }
#endif
  uint64_t ret = distance(input_min,fut);
  fut->orig_distances = fut->distances;
  return ret;
  return distance(input_min,fut);
}




bool gd_entry(struct FUT* fut) {
  MutInput input(fut->inputs.size());
  MutInput scratch_input(fut->inputs.size());
  //return true;

  uint64_t f0 = reload_input(input,fut); 

  if (f0 == UINTMAX_MAX)
    return false;

  int ep_i = 0;

  Grad grad(input.len());

  while (true) {
    //std::cout << "<<< epoch=" << ep_i << " f0=" << f0 << std::endl;
    if (fut->stopped) {
      break;
    }
    cal_gradient(input, f0, grad,fut);

    int g_i = 0;

    while (grad.max_val() == 0) {
      if (g_i > MAX_NUM_MINIMAL_OPTIMA_ROUND) {
        break;
      }
      if (fut->stopped)
        break;
      g_i++;
      //f0 = repick_start_point(input, f0, rng);
      //f0 = reload_input(input);
      f0 = repick_start_point(input,fut);
      if (fut->stopped)
        break;
      grad.clear();
      cal_gradient(input,f0,grad,fut);
    }
    if (fut->stopped || g_i > MAX_NUM_MINIMAL_OPTIMA_ROUND) {
      //std::cout << "trapped in local optimia for too long" << std::endl;
      break;
    }
    //TODO
    grad.normalize();
    f0 = descend(input, scratch_input,f0, grad,fut);
    ep_i += 1;
  }

  return fut->gsol;
}
