#ifndef TEST_H_
#define TEST_H_
#include <stdint.h>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include <bitset>
//function under test
//constraint: 0 = equal, 1 = distinct, 2 = lt, 3 = le, 4 = gt, 5 = ge 
typedef uint64_t(*test_fn_type)(uint64_t*);
void load_input(std::vector<std::pair<uint32_t,uint8_t>> &inputs, std::string input_file);

class Constraint {
public:
	test_fn_type fn;
	uint32_t comparison;

	//map the offset to the idx in inputs_args
	// if const {false, const value}, if symbolic {true, index in the inputs}
	std::vector<std::pair<bool, uint64_t>> input_args_scratch;
	std::unordered_map<uint32_t,uint32_t> local_map;
	//map the offset to iv
	std::unordered_map<uint32_t,uint8_t> inputs;
	uint32_t const_num;
};


class ConsMeta {
public:
	std::vector<std::pair<bool, uint64_t>> input_args_final;
  uint32_t index;
};

struct FUT {  
	FUT(): scratch_args(nullptr), max_const_num(0) {}
	~FUT() { if (scratch_args) free(scratch_args); }
	uint32_t num_exprs;
	std::vector<std::shared_ptr<Constraint>> constraints;
  std::vector<std::shared_ptr<ConsMeta>> constraintsmeta;

	// offset and input value
	std::vector<std::pair<uint32_t,uint8_t>> inputs;

	uint64_t start; //start time
	uint32_t max_const_num;
	bool stopped = false;
	int att = 0;
	int num_minimal_optima = 0;
	bool gsol = false;
	bool opti_hit = false;
	std::unordered_map<uint32_t,uint8_t> *rgd_solution;
	std::unordered_map<uint32_t,uint8_t> *opti_solution;
	std::unordered_map<uint32_t,uint8_t> *hint_solution;  
  std::vector<uint64_t> orig_distances;
  std::vector<uint64_t> distances;
	uint64_t* scratch_args;

  std::unordered_map<uint32_t, std::vector<uint32_t>> cmap;
	//void allocate_scratch_args(int size) {scratch_args = (uint8_t*)aligned_alloc(64,size);}
	void finalize() {
	  //aggregate the contraints, fill input_args's index, build global inputs
    
		std::unordered_map<uint32_t,uint32_t> sym_map;
		uint32_t gidx = 0;
		for (size_t i =0; i< constraints.size(); i++) {
      std::shared_ptr<ConsMeta> meta = std::make_shared<ConsMeta>();
      meta->index = i;
      meta->input_args_final = constraints[i]->input_args_scratch;
			for (auto itr : constraints[i]->local_map) {
				auto gitr = sym_map.find(itr.first);
				if (gitr == sym_map.end()) {
					gidx = inputs.size();
					sym_map[itr.first] = gidx;
					inputs.push_back(std::make_pair(itr.first,constraints[i]->inputs[itr.first]));
          auto slot = cmap.find(gidx);
          if (slot != cmap.end()) {
            slot->second.push_back(i);
          } else {
            std::vector<uint32_t> a;
            a.push_back(i);
            cmap.insert({gidx, a});
          }
          //cmap[gidx].push_back(constraints[i]);
				} else {
					gidx = gitr->second;
          auto slot = cmap.find(gidx);
          if (slot != cmap.end()) {
            slot->second.push_back(i);
          } else {
            std::vector<uint32_t> a;
            a.push_back(i);
            //cmap.insert({gidx, a});
            cmap[gidx] = a;
          }
          //cmap[gidx].push_back(constraints[i]);
				}
				meta->input_args_final[itr.second].second = gidx;  //update input_args
			}
      constraintsmeta.push_back(meta);
		}

		
		for (size_t i=0; i < constraints.size(); i++) {
			if (max_const_num < constraints[i]->const_num)
				max_const_num = constraints[i]->const_num;
		}
		//reserver 2 for comparison operands a,b
		scratch_args = (uint64_t*)malloc((2 + inputs.size() + max_const_num + 100) * sizeof(uint64_t));
    orig_distances.resize(constraints.size(), 0); 
    distances.resize(constraints.size(), 0); 
	}


  void load_hint() {// load hint
    for(auto itr = inputs.begin(); itr!=inputs.end();itr++) {
      auto got = hint_solution->find(itr->first);
      if (got != hint_solution->end()) 
        itr->second = got->second; 
    }
  }
  void load_buf(std::string file_name) {// load hint
    load_input(inputs, file_name);
  }


};
#endif
