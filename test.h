#ifndef TEST_H_
#define TEST_H_
#include <stdint.h>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include <bitset>

#include "grad.h"
#include "input.h"

// function under test
// constraint: 0 = equal, 1 = distinct, 2 = lt, 3 = le, 4 = gt, 5 = ge 
typedef void(*test_fn_type)(uint64_t*);

//the first two slots of the arguments for reseved for the left and right operands
static const int RET_OFFSET = 2;

struct Constraint {
  // JIT'ed function for a comparison expression
  test_fn_type fn;
  // the relational operator
  uint32_t comparison;

  // During constraint collection, (symbolic) input bytes are recorded
  // as offsets from the beginning of the input.  However, the JIT'ed
  // function consumes inputs as an input array.  So, when building the
  // function, we need to map the offset to the idx in input array,
  // which is stored in local_map.
  std::map<uint32_t, uint32_t> local_map;
  // if const {false, const value}, if symbolic {true, index in the inputs}
  // during local search, we use a single global array (to avoid memory
  // allocation and free) to prepare the inputs, so we need to know where
  // to load the input values into the input array.
  std::vector<std::pair<bool, uint64_t>> input_args;
  // map the offset to iv (initial value)
  std::unordered_map<uint32_t, uint8_t> inputs;
  // shape information about the input (e.g., 1, 2, 4, 8 bytes)
  std::unordered_map<uint32_t, uint32_t> shapes;
  // number of constant in the input array
  uint32_t const_num;
};


struct ConsMeta {
  // per-FUT arg mapping, so we can share the constraints
  std::vector<std::pair<bool, uint64_t>> input_args;
  // input2state inference related
  bool i2s_feasible;
  uint64_t op1, op2;
};

struct FUT {
  FUT(): scratch_args(nullptr), max_const_num(0) {}
  ~FUT() { if (scratch_args) free(scratch_args); }
  uint32_t num_exprs;
  // constraints, could be shared, strictly read-only
  std::vector<std::shared_ptr<const Constraint>> constraints;
  // per-FUT mutable metadata of constraints
  std::vector<std::unique_ptr<ConsMeta>> consmeta;

  // inputs as pairs of <offset (from the beginning of the input, and value>
  std::vector<std::pair<uint32_t, uint8_t>> inputs;
  // shape information at each offset
  std::unordered_map<uint32_t, uint32_t> shapes;
  // max number of constants in the input array
  uint32_t max_const_num;
  // record constraints that use a certain input byte
  std::unordered_map<uint32_t, std::vector<size_t>> cmap;
  // the input array used for all JIT'ed functions
  // all input bytes are extended to 64 bits
  uint64_t* scratch_args;

  // intermediate states for the search
  std::vector<uint64_t> orig_distances;
  std::vector<uint64_t> distances;

  // statistics
  uint64_t start; //start time
  bool stopped = false;
  int attempts = 0;
  int num_minimal_optima = 0;
  bool gsol = false;
  bool opti_hit = false;

  // solutions
  std::unordered_map<uint32_t, uint8_t> *rgd_solution;
  std::unordered_map<uint32_t, uint8_t> *opti_solution;
  std::unordered_map<uint32_t, uint8_t> *hint_solution;

  void finalize() {
    // aggregate the contraints, map each input byte to a constraint to
    // an index in the "global" input array (i.e., the scratch_args)
    std::unordered_map<uint32_t, uint32_t> sym_map;
    uint32_t gidx = 0;
    for (size_t i = 0; i < constraints.size(); i++) {
      std::unique_ptr<ConsMeta> cm = std::make_unique<ConsMeta>();
      cm->input_args = constraints[i]->input_args;
      uint32_t last_offset = -1;
      cm->i2s_feasible = true;
      for (const auto& [offset, lidx] : constraints[i]->local_map) {
        auto gitr = sym_map.find(offset);
        if (gitr == sym_map.end()) {
          gidx = inputs.size();
          sym_map[offset] = gidx;
          inputs.push_back(std::make_pair(offset, constraints[i]->inputs.at(offset)));
          shapes[offset] = constraints[i]->shapes.at(offset);
        } else {
          gidx = gitr->second;
        }
        auto slot = cmap.find(gidx);
        if (slot != cmap.end()) {
          slot->second.push_back(i);
        } else {
          cmap.emplace(std::make_pair(gidx, std::vector<size_t>{i}));
        }
        // save the mapping between the local index (i.e., where the JIT'ed
        // function is going to read the input from) and the global index
        // (i.e., where the current value corresponding to the input byte
        // is stored in MutInput)
        cm->input_args[lidx].second = gidx;

        // check if the input bytes are consecutive
        // using std::map ensures that the offsets (keys) are sorted
        if (last_offset != -1 && last_offset + 1 != offset) {
          cm->i2s_feasible = false;
        }
        last_offset = offset;
      }
      // FIXME: only support up to 64-bit for now
      if (constraints[i]->local_map.size() > 8) {
        cm->i2s_feasible = false;
      }

      // update the number of required constants in the input array
      if (max_const_num < constraints[i]->const_num)
        max_const_num = constraints[i]->const_num;

      // insert the constraint metadata
      consmeta.push_back(std::move(cm));
    }

    // allocate the input array, reserver 2 for comparison operands a,b
    scratch_args = (uint64_t*)aligned_alloc(sizeof(*scratch_args),
        (2 + inputs.size() + max_const_num + 1) * sizeof(*scratch_args));
    orig_distances.resize(constraints.size(), 0);
    distances.resize(constraints.size(), 0);
  }

  void load_hint() { // load hint
    for (auto itr = inputs.begin(); itr != inputs.end(); itr++) {
      auto got = hint_solution->find(itr->first);
      if (got != hint_solution->end()) 
        itr->second = got->second; 
    }
  }

};

#endif // TEST_H_