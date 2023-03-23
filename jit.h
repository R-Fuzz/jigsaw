#ifndef JIT_H_
#define JIT_H_

#include "rgd.pb.h"
#include "test.h"

int addFunction(const rgd::JitRequest* request,
    std::unordered_map<uint32_t, uint32_t> &local_map,
    uint64_t id,
    std::unordered_map<uint32_t, rgd::JitRequest*> &expr_cache);

test_fn_type performJit(uint64_t id);

#endif