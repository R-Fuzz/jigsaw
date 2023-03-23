#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/thread/thread_pool.hpp>
#include <gperftools/profiler.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <boost/filesystem.hpp>

#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

#include "rgdJit.h"
#include "ctpl.h"
#include "rgd_op.h"
#include "jit.h"
#include "wheels/lockfreehash/lprobe/hash_table.h"
#include "wheels/concurrentqueue/queue.h"
#include "util.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h> /* mmap() is defined in this header */
#include "rgd.pb.h"


#include "test.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
#define DEBUG 0
#define CONSTRAINT_CACHE 1
#define CHECK_DIS 0
#define CODEGEN_V2 1
#define THREAD_POOL_SIZE 0
#define SINGLE_BRANCH 0
#define NESTED_BRANCH 1
#define LOADING_LIMIT 1000000
#define PROCESSING_LIMIT 1000000

using namespace pbbs;
using namespace llvm;
using namespace llvm::orc;
using namespace google::protobuf::io;
using namespace boost::filesystem;
using namespace rgd;

int core_start = 0;
//llvm::orc::ThreadSafeContext TSCtx(llvm::make_unique<llvm::LLVMContext>());
//inline llvm::LLVMContext& getContext() { return *TSCtx.getContext(); }
//std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<GradJit> JIT;// = make_unique<GradJit>();
uint64_t getTimeStamp();
uint64_t start_time;

uint8_t input_buffer[64][8092];

bool gd_entry(struct FUT* dut);

namespace rgd {

bool recursive_equal(const JitRequest& lhs, const JitRequest& rhs) {
  if ((lhs.kind() >= rgd::Ult && lhs.kind() <= rgd::Uge) && 
      (rhs.kind() >= rgd::Ult && rhs.kind() <= rgd::Uge)) {
    const int children_size = lhs.children_size();
    if (children_size != rhs.children_size()) return false;
    for (int i = 0; i < children_size; i++) {
      if (!recursive_equal(lhs.children(i), rhs.children(i)))
        return false;
    }
    return true;
  }
  if ((lhs.kind() >= rgd::Slt && lhs.kind() <= rgd::Sge) && 
      (rhs.kind() >= rgd::Slt && rhs.kind() <= rgd::Sge)) {
    const int children_size = lhs.children_size();
    if (children_size != rhs.children_size()) return false;
    for (int i = 0; i < children_size; i++) {
      if (!recursive_equal(lhs.children(i), rhs.children(i)))
        return false;
    }
    return true;
  }
  if ((lhs.kind() >= rgd::Equal && lhs.kind() <= rgd::Distinct) && 
      (rhs.kind() >= rgd::Equal && rhs.kind() <= rgd::Distinct)) {
    const int children_size = lhs.children_size();
    if (children_size != rhs.children_size()) return false;
    for (int i = 0; i < children_size; i++) {
      if (!recursive_equal(lhs.children(i), rhs.children(i)))
        return false;
    }
    return true;
  }
  if (lhs.hash() != rhs.hash()) return false;
  if (lhs.kind() != rhs.kind()) return false;
  if (lhs.bits() != rhs.bits()) return false;
  const int children_size = lhs.children_size();
  if (children_size != rhs.children_size()) return false;
  for (int i = 0; i < children_size; i++) {
    if (!recursive_equal(lhs.children(i), rhs.children(i)))
      return false;
  }
  return true;
}

bool isEqual(const JitRequest& lhs, const JitRequest& rhs) {
  return recursive_equal(lhs, rhs);
}

struct myKV {
  std::shared_ptr<JitRequest> req;
  test_fn_type fn;
  myKV(std::shared_ptr<JitRequest> areq, test_fn_type f) : req(areq), fn(f) {}
};

struct nestKV {
  uint32_t label;
  std::shared_ptr<JitRequest> req;
  nestKV(uint32_t l, std::shared_ptr<JitRequest> areq) : label(l), req(areq) {}
};

struct consKV {
  std::tuple<uint32_t,uint32_t,uint32_t> label;
  std::shared_ptr<Constraint> cons;
  consKV(std::tuple<uint32_t,uint32_t,uint32_t> l, std::shared_ptr<Constraint> acon) : label(l), cons(acon) {}
};

struct myHash {
  using eType = struct myKV*;
  using kType = std::shared_ptr<JitRequest>;
  eType empty() {return nullptr;}
  kType getKey(eType v) {return v->req;}
  int hash(kType v) {return v->hash();} //hash64_2(v);}
  //int hash(kType v) {return hash64_2(v);}
  //int cmp(kType v, kType b) {return (v > b) ? 1 : ((v == b) ? 0 : -1);}
  int cmp(kType v, kType b) {return (isEqual(*v,*b)) ? 0 : -1;}
  bool replaceQ(eType, eType) {return 0;}
  eType update(eType v, eType) {return v;}
  bool cas(eType* p, eType o, eType n) {return atomic_compare_and_swap(p, o, n);}
};

struct nestHash {
  using eType = struct nestKV*;
  using kType = uint32_t;
  eType empty() {return nullptr;}
  kType getKey(eType v) {return v->label;}
  int hash(kType v) {return v;} //hash64_2(v);}
  //int hash(kType v) {return hash64_2(v);}
  //int cmp(kType v, kType b) {return (v > b) ? 1 : ((v == b) ? 0 : -1);}
  int cmp(kType v, kType b) {return (v==b) ? 0 : -1;}
  bool replaceQ(eType, eType) {return 0;}
  eType update(eType v, eType) {return v;}
  bool cas(eType* p, eType o, eType n) {return atomic_compare_and_swap(p, o, n);}
};

static inline uint32_t xxhash(uint32_t h1, uint32_t h2, uint32_t h3) {
  const uint32_t PRIME32_1 = 2654435761U;
  const uint32_t PRIME32_2 = 2246822519U;
  const uint32_t PRIME32_3 = 3266489917U;
  const uint32_t PRIME32_4 =  668265263U;
  const uint32_t PRIME32_5 =  374761393U;

#define XXH_rotl32(x,r) ((x << r) | (x >> (32 - r)))
  uint32_t h32 = PRIME32_5;
  h32 += h1 * PRIME32_3;
  h32  = XXH_rotl32(h32, 17) * PRIME32_4;
  h32 += h2 * PRIME32_3;
  h32  = XXH_rotl32(h32, 17) * PRIME32_4;
  h32 += h3 * PRIME32_3;
  h32  = XXH_rotl32(h32, 17) * PRIME32_4;
#undef XXH_rotl32

  h32 ^= h32 >> 15;
  h32 *= PRIME32_2;
  h32 ^= h32 >> 13;
  h32 *= PRIME32_3;
  h32 ^= h32 >> 16;

  return h32;
}

struct consHash {
  using eType = struct consKV*;
  using kType =	std::tuple<uint32_t,uint32_t,uint32_t>;
  eType empty() {return nullptr;}
  kType getKey(eType v) {return v->label;}
  //int hash(kType v) {return std::get<0>(v) ^ std::get<1>(v) ^ std::get<2>(v);} //hash64_2(v);}
  int hash(kType v) { return xxhash(std::get<0>(v), std::get<1>(v), std::get<2>(v));} //hash64_2(v);}
  //int hash(kType v) {return hash64_2(v);}
  //int cmp(kType v, kType b) {return (v > b) ? 1 : ((v == b) ? 0 : -1);}
  int cmp(kType v, kType b) {return (std::get<0>(v) == std::get<0>(b) && std::get<1>(v) == std::get<1>(b) && std::get<2>(v) == std::get<2>(b)) ? 0 : -1;}
  bool replaceQ(eType, eType) {return 0;}
  eType update(eType v, eType) {return v;}
  bool cas(eType* p, eType o, eType n) {return atomic_compare_and_swap(p, o, n);}
};

struct RequestHash {
  std::size_t operator()(const JitRequest& req) const {
    return req.hash();
  }
};

struct RequestEqual {
  bool recursive_equal(const JitRequest& lhs, const JitRequest& rhs) const {
    if (lhs.hash() != rhs.hash()) return false;
    if (lhs.kind() != rhs.kind()) return false;
    if (lhs.bits() != rhs.bits()) return false;
    const int children_size = lhs.children_size();
    if (children_size != rhs.children_size()) return false;
    for (int i = 0; i < children_size; i++) {
      if (!recursive_equal(lhs.children(i), rhs.children(i)))
        return false;
    }
    return true;
  }

  bool operator()(const JitRequest& lhs, const JitRequest& rhs) const {
    return recursive_equal(lhs, rhs);
  }
};

typedef std::tuple<uint64_t, uint64_t,bool> trace_context;
struct context_hash {
  std::size_t operator()(const trace_context &context) const {
    return std::hash<uint64_t>{}(std::get<0>(context)) ^ std::hash<uint64_t>{}(std::get<1>(context)) ^ std::hash<bool>{}(std::get<2>(context));
  }
};


class RGDServiceImpl {

  public:
    RGDServiceImpl();
};

static std::atomic<uint64_t> jigsaw_queue_size;
static std::atomic<uint64_t> z3_queue_size;
static std::atomic<uint64_t> uuid;
static std::atomic<uint64_t> ufid;
static std::atomic<uint64_t> misssubexprs;

static pbbs::Table<myHash> fCache(8000016,myHash(),1.3);
static pbbs::Table<nestHash> nestCache(8000016,nestHash(),1.3);
static pbbs::Table<consHash> consCache(8000016,consHash(),1.3);
static std::mutex gg_mutex;
static std::atomic<uint64_t> hit;
static std::atomic<uint64_t> miss;

static std::atomic<uint64_t> gProcessed;
static std::atomic<uint64_t> gFi;
static std::atomic<uint64_t> skipped;

static std::atomic<uint64_t> solving_total;
static std::atomic<uint64_t> parsing_total;
static std::atomic<uint64_t> parsing1_total;
static std::atomic<uint64_t> parsing2_total;
static std::atomic<uint64_t> parsing_total3;
static std::atomic<uint64_t> iterations_total;
static std::atomic<uint64_t> gCmdIdx(0);

} // namespace rgd


static void dumpFut(FUT* fut) {
  std::cout << "dumping fut start -------------" << std::endl;
  //for(std::unordered_map<uint32_t,uint32_t>::iterator it=fut->global_map.begin();it!=fut->global_map.end();it++)
  //	std::cout << "first is " << it->first << " second is " << it->second << std::endl; 
  for (auto c : fut->constraints) {
    printf("constraint comparsison is %d\n",c->comparison);
  }
  for (auto c: fut->inputs) {
    printf("offset %u and value %u\n",c.first,c.second);
  }
  std::cout << "dumping fut end-------------" << std::endl;
}


RGDServiceImpl::RGDServiceImpl() {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  JIT = std::move(GradJit::Create().get());

#if THREAD_POOL_SIZE
  //pool = new ctpl::thread_pool(THREAD_POOL_SIZE);
#endif
}


static void analyzeExpr(JitRequest* request, bool &hasIte, bool &abWidth, int depth, bool &nonRootLNot, bool &div, bool &hasZExt, bool &zext_bool, std::unordered_map<uint32_t, JitRequest*> &expr_cache) {
  auto r1 = expr_cache.find(request->label());

  if (request->label() != 0 && r1 == expr_cache.end())
    expr_cache.insert({request->label(),request});
  else if (request->label() != 0 &&
      r1 != expr_cache.end()) {
    request = expr_cache[request->label()];
  }

  if (request->bits() > 64) abWidth = true;

  if (request->kind() == rgd::Ite) {
    hasIte = true;
  } else if (request->kind() == rgd::ZExt) {
    hasZExt = true;
  } /*else if (request->kind() == rgd::UDiv
      || request->kind() == rgd::SDiv
      || request->kind() == rgd::SRem
      || request->kind() == rgd::URem) {
      div = true;
      } */ else if (isRelational(request->kind())) {
        if (hasZExt)
          zext_bool = true;
      } else if (request->kind() == rgd::LNot) {
        if (depth != 0)
          nonRootLNot = true;
      }

for (int i = 0; i < request->children_size(); i++)
  analyzeExpr(request->mutable_children(i), hasIte, abWidth, ++depth, nonRootLNot, div, hasZExt, zext_bool, expr_cache);
}

//global map: request->index() -> index in the Inputs. Inputs maps offset -> iv or 0
// input_args: false, iv for const  / true, gidx for index in the  inputs
static void mapArgs(JitRequest* req, 
    std::shared_ptr<Constraint> constraint,
    std::unordered_set<uint32_t> &visited) {

  for (int i=0; i < req->children_size(); i++) {
    auto child = req->mutable_children(i);
    uint32_t label = child->label();
    if (label!=0 && visited.count(label)==1)
      continue;
    visited.insert(label);
    mapArgs(child, constraint ,visited);
  }
  XXH32_state_t state;
  XXH32_reset(&state,0);
  uint32_t hash = 0;
  uint32_t bits = req->bits();
  uint32_t kind = req->kind();
  //XXH32_update(&state, &bits, sizeof(bits));
  //if (req->kind() < rgd::Equal || req->kind() > rgd::Sge)
    //XXH32_update(&state, &kind, sizeof(kind));
  if (req->kind() == rgd::Constant) {
    uint32_t start = (uint32_t)constraint->input_args.size();
    req->set_index(start);  //save index
    llvm::StringRef ref(req->value());
    llvm::APInt value(req->bits(), ref, 10);
    uint64_t iv = value.getZExtValue();
    constraint->input_args.push_back(std::make_pair(false,iv));
    constraint->const_num += 1;
    //build index by local index
    //XXH32_update(&state,&start,sizeof(start));
    //req->set_hash(XXH32_digest(&state));
    hash = xxhash(bits, kind, start);
    req->set_hash(hash);
  }
  else if (req->kind() == rgd::Read) {
    //search from current input
    uint64_t iv = 0;
    if (!req->value().empty()) {
      llvm::StringRef ref(req->value());
      llvm::APInt value(req->bits(),ref,10);
      iv = value.getZExtValue();
    }
    size_t length = req->bits()/8;
    for (int i=0;i<length;++i, iv>>=8) {
      uint32_t offset = req->index() + i;
      uint32_t arg_index = 0;
      auto itr = constraint->local_map.find(offset);
      if ( itr == constraint->local_map.end()) {
        arg_index = (uint32_t)constraint->input_args.size();
        constraint->inputs.insert({offset,(uint8_t)(iv & 0xff)});
        constraint->local_map[offset] = arg_index;
        constraint->input_args.push_back(std::make_pair(true,0)); // 0 is to be filled in the aggragation
      } else {
        arg_index = itr->second;
      }
      //if (i==0) {
      //XXH32_update(&state,&arg_index,sizeof(arg_index));
      //req->set_hash(XXH32_digest(&state));

      hash = xxhash(bits, kind, arg_index);
      req->set_hash(hash);
      // }
    }
  } else {
    if (req->kind() < rgd::Equal || req->kind() > rgd::Sge)
    //XXH32_update(&state, &kind, sizeof(kind));
      hash = xxhash(bits, kind, 0);
    for (int32_t i = 0; i < req->children_size(); i++) {
      uint32_t h = req->children(i).hash();
      //XXH32_update(&state,&h,sizeof(h));
      hash = xxhash(hash, h, 0);
    }
    //req->set_hash(XXH32_digest(&state));
    req->set_hash(hash);
  }
}


//global map: request->index() -> index in the MutInput::value
//local map:  request->index() -> indices in the fut->scatch_args
//constant_values: u8 value -> indices in the fut->scratch_args
static void analyzeExpr(JitRequest* request, 
    std::map<uint32_t, std::vector<uint32_t>> &local_map,
    std::map<uint32_t, uint32_t> &global_map,
    uint32_t &last_offset,
    std::vector<std::pair<uint8_t,uint32_t>> &constant_values,
    bool &hasIte, bool &abWidth, int depth, bool &nonRootLNot,
    bool &div, bool &hasZExt, bool &zext_bool,
    std::unordered_map<uint32_t, JitRequest*> &expr_cache) {

  auto r1 = expr_cache.find(request->label());

  if (request->label() != 0 && r1 == expr_cache.end())
    expr_cache.insert({request->label(),request});
  else if (request->label() != 0 &&
      r1 != expr_cache.end()) {
    request = expr_cache[request->label()];
  }

  if (request->bits() > 64) abWidth = true;

  //Constant mapping
  if (request->kind() == rgd::Constant) {
    //request->set_index(last_offset);  //we abuse the index field of constant to store offsets in the input array  //we don't need this
    uint32_t start = last_offset;
    uint32_t length = request->bits() / 8;
    last_offset += length;
    llvm::StringRef ref(request->value());
    llvm::APInt value(request->bits(), ref, 10);
    uint64_t iv = value.getZExtValue();
    uint8_t buf[8];
    memcpy(buf, &iv, length);
    for(uint32_t i = 0; i < length; i++) {
      // std::cout<<(int)buf[i]<<", ";
      constant_values.push_back({buf[i],start+i});
    }
    // std::cout<<std::endl;
  } else if (request->kind() == rgd::Read) {
    //llvm::StringRef ref(request->value());
    //llvm::APInt value(request->bits(), ref, 10);
    //uint8_t iv = (uint8_t)value.getZExtValue();
    size_t length = request->bits()/8;
    uint32_t start = last_offset;
    for (int i=0;i<length;++i) {
      if (global_map.find(request->index()+i) == global_map.end()){
        size_t gsize = global_map.size();
        global_map[request->index()+i] = gsize;
      }
      if (local_map.find(request->index()+i) == local_map.end()) {
        //size_t lsize = local_map.size();
        local_map[request->index()+i] = {start+i};
      } else
        local_map[request->index()+i].push_back(start+i);
    }
    last_offset += length;
  } else if (request->kind() == rgd::Ite) {
    hasIte = true;
  } else if (request->kind() == rgd::ZExt) {
    hasZExt = true;
  }  /*else if (request->kind() == rgd::UDiv
       || request->kind() == rgd::SDiv
       || request->kind() == rgd::SRem
       || request->kind() == rgd::URem) {
       div = true;
       }  */ else if (isRelational(request->kind())) {
         if (hasZExt)
           zext_bool = true;
       } else if (request->kind() == rgd::LNot) {
         if (depth != 0)
           nonRootLNot = true;
       }

for (int i = 0; i < request->children_size(); i++)
  analyzeExpr(request->mutable_children(i), local_map, global_map, last_offset,
      constant_values, hasIte, abWidth, ++depth, nonRootLNot, div, hasZExt, zext_bool ,expr_cache);
}

static const JitRequest* transform(const JitRequest* request, int &constraint) {
  switch (request->kind()) {
    case rgd::Equal:
      constraint = 0;
      break;
    case rgd::Distinct:
      constraint = 1; 
      break;
    case rgd::Ult:
      constraint = 2; 
      break;
    case rgd::Ule:
      constraint = 3; 
      break;
    case rgd::Ugt:
      constraint = 4; 
      break;
    case rgd::Uge:
      constraint = 5; 
      break;
    case rgd::Slt:
      constraint = 2; 
      break;
    case rgd::Sle:
      constraint = 3; 
      break;
    case rgd::Sgt:
      constraint = 4; 
      break;
    case rgd::Sge:
      constraint = 5; 
      break;
    case rgd::LNot: {
      switch (request->children(0).kind()) {
        case rgd::Equal:
          constraint = 1;
          break;
        case rgd::Distinct:
          constraint = 0;
          break;
        case rgd::Ult:
          constraint = 5; 
          break;
        case rgd::Ule:
          constraint = 4;
          break;
        case rgd::Ugt:
          constraint = 3;
          break;
        case rgd::Uge:
          constraint = 2;
          break;
        case rgd::Slt:
          constraint = 5;
          break;
        case rgd::Sle:
          constraint = 4;
          break;
        case rgd::Sgt:
          constraint = 3;
          break;
        case rgd::Sge:
          constraint = 2;
          break;
        default:
          //std::cerr << "unhandled child kind " << request->children(0).kind() << std::endl; 
          return nullptr;
      }
      return &request->children(0);
    }
    default:
      // all subexpr must be a comparison (conditional branch)
      //std::cerr << "unhandled kind " << request->kind() << std::endl; 
      return nullptr;
  }
  return request;
}

static void dumpList(std::deque<std::deque<const JitRequest*>> list) {
  std::cout << "dump list" << std::endl;
  for(auto l : list) {
    for(auto i : l)
      printExpression(i);
    std::cout<<std::endl;
  }
}

static void toDNF(JitRequest* req, std::deque<std::deque<JitRequest*>> &res) {
  if (req->kind() == rgd::LAnd) {
    std::deque<std::deque<JitRequest*>> p1;
    std::deque<std::deque<JitRequest*>> p2;
    toDNF(req->mutable_children(0), p1);
    toDNF(req->mutable_children(1), p2);
    for (auto &sub1 : p1) {
      for (auto &sub2: p2) {
        std::deque<JitRequest*> cur;
        cur.insert(cur.end(), sub1.begin(), sub1.end());
        cur.insert(cur.end(), sub2.begin(), sub2.end());
        res.push_back(cur);
      }
    }
    if(p1.size()==0) res = p2;
  } else if (req->kind() == rgd::LOr) {
    //copy list
    toDNF(req->mutable_children(0), res);
    toDNF(req->mutable_children(1), res);
  } else {
    std::deque<JitRequest*> cur;
    cur.push_back(req);
    res.push_back(cur);
  }
}

static bool rejectTask(std::deque<JitRequest*> &list) {
  bool hasIte = false;
  bool abWidth = false;
  int depth = 0;
  bool nonRootLNot = false;
  bool div = false;
  bool hasZExt = false;
  bool zext_bool = false;


  // collect input and const arguments
  std::unordered_map<uint32_t, JitRequest*> expr_cache;
  for (auto expr : list) {
    analyzeExpr(expr, hasIte, abWidth, depth, nonRootLNot,div,hasZExt, zext_bool, expr_cache);
    if (hasIte || abWidth || nonRootLNot || div || zext_bool) {
      ++skipped;
      return true;
    }
  }
  return false;
}

static JitRequest* negate(JitRequest* request) {
  JitRequest* c = request;
  switch (c->kind()) {
    case rgd::Equal:
      c->set_kind(rgd::Distinct);
      break;
    case rgd::Distinct:
      c->set_kind(rgd::Equal);
      break;
    case rgd::Ult:
      c->set_kind(rgd::Uge);
      break;
    case rgd::Ule:
      c->set_kind(rgd::Ugt);
      break;
    case rgd::Ugt:
      c->set_kind(rgd::Ule);
      break;
    case rgd::Uge:
      c->set_kind(rgd::Ult);
      break;
    case rgd::Slt:
      c->set_kind(rgd::Sge);
      break;
    case rgd::Sle:
      c->set_kind(rgd::Sgt);
      break;
    case rgd::Sgt:
      c->set_kind(rgd::Sle);
      break;
    case rgd::Sge:
      c->set_kind(rgd::Slt);
      break;
    default:
      return nullptr;
  }
  return c;
}


static JitRequest* simplify(JitRequest* request) {
  // strip LNot
  if (request->kind() == rgd::LNot)
    request = negate(request->mutable_children(0));
  if (!request)
    return nullptr;

  // strip unnecessary comparison
  // distinct(zext(32, equal(zext(64, read(0)), constant(13))), constant(0))
  // => equal(zext(64, read(0)), constant(13))
  if (request->kind() == rgd::Distinct || request->kind() == rgd::Equal) {
    auto c0 = request->mutable_children(0);
    auto c1 = request->mutable_children(1);
    if (c1->kind() == rgd::ZExt && c0->kind() == rgd::Constant) {
      // swap
      auto tmp = c1;
      c1 = c0;
      c0 = tmp;
    }
    if (c0->kind() == rgd::ZExt && c1->kind() == rgd::Constant) {
      auto c00 = c0->mutable_children(0);
      if (isRelational(c00->kind())) {
        llvm::StringRef ref(c1->value());
        llvm::APInt value(request->bits(), ref, 10);
        uint64_t cv = value.getZExtValue();
        if (request->kind() == rgd::Distinct) {
          if (cv == 0) {
            // != 0 => true => keep the same
            request = c00;
          } else {
            // != 1 => false => negate
            request = negate(c00);
          }
        } else { // rgd::Equal
          if (cv == 0) {
            // == 0 => false => negate
            request = negate(c00);
          } else {
            // == 1 => true => keep the same
            request = c00;
          }
        }
      }
    }
  }
  // strip LNot
  if (request->kind() == rgd::LNot)
    request = negate(request->mutable_children(0));

  return request;
}

struct temp_hash {
  std::size_t operator()(const std::tuple<uint32_t,uint32_t,uint32_t> &dd) const {
    return std::get<0>(dd) ^ std::get<1>(dd) ^ std::get<2>(dd);
  }
};

struct temp_equal {
  bool operator()(const std::tuple<uint32_t,uint32_t,uint32_t> &lhs,
      const std::tuple<uint32_t,uint32_t,uint32_t> &rhs) const {
    return std::get<0>(lhs) == std::get<0>(rhs) && std::get<1>(lhs) == std::get<1>(rhs) && std::get<2>(lhs) == std::get<2>(rhs);
  }
};
std::unordered_map<std::tuple<uint32_t,uint32_t,uint32_t>, std::shared_ptr<Constraint>, temp_hash,temp_equal> temp(1000000);

static FUT* constructTask(std::deque<JitRequest*> &list, int threadId,
    std::unordered_map<JitRequest, uint64_t, RequestHash, RequestEqual> &funcCache) {
  struct FUT *fut = new FUT();
  //GradJit *JIT = new GradJit();
  fut->gsol = false;
  fut->attempts = 0;
  fut->stopped = false;
  fut->num_minimal_optima = 0;
  static int localhit=0;
  static int localmiss=0;


  uint64_t start = getTimeStamp();
  // collect input and const arguments
  for (auto expr : list) {
    std::unordered_map<uint32_t, JitRequest*> expr_cache;
    if (expr->kind() == rgd::Constant) continue;
    JitRequest* adjusted_request = nullptr;
    adjusted_request = simplify(expr);
    if (!adjusted_request) {
      delete fut;
      return nullptr;
    }

    if(!isRelational(adjusted_request->kind())) {
      printf("no relational!\n");
      return nullptr;
    }

    //printf("adjust session id %d, label %d, kind %d\n", adjusted_request->sessionid(), adjusted_request->label(), adjusted_request->kind());
#if CONSTRAINT_CACHE
    uint64_t time2 = getTimeStamp();
    struct consKV *res1 = consCache.find({adjusted_request->sessionid(), adjusted_request->label(), adjusted_request->kind()});
    parsing_total3 += getTimeStamp() - time2;
    if (res1) {
      //printf("local hit %d\n", ++localhit);
      std::shared_ptr<Constraint> constraint1 = res1->cons;
      fut->constraints.push_back(constraint1);
      continue;
    } else {
      //printf("local miss %d\n", ++localmiss);
    }
#endif

    std::unordered_set<uint32_t> visited;
    std::shared_ptr<Constraint> constraint = std::make_shared<Constraint>();
    constraint->const_num = 0;
    constraint->comparison = adjusted_request->kind();
    mapArgs(adjusted_request,constraint,visited);
    std::shared_ptr<JitRequest> copied_req = std::make_shared<JitRequest>();
    copied_req->CopyFrom(*adjusted_request);

    struct myKV *res = fCache.find(copied_req);
    //  struct myKV *res = nullptr;
    if (res == nullptr) {
      miss++;
      uint64_t id = ++uuid;
      addFunction(adjusted_request, constraint->local_map, id, expr_cache);
      auto fn = performJit(id);
      if (!fCache.insert(new struct myKV(copied_req, fn))) {
        delete res;
        res = nullptr;
      }
      constraint->fn = fn;
    } else {
      hit++;
      constraint->fn = res->fn;
    }

    assert(isRelational(adjusted_request->kind()) && "non-relational expr");
    fut->constraints.push_back(constraint);
    consCache.insert(new struct consKV({adjusted_request->sessionid(), adjusted_request->label(), adjusted_request->kind()},constraint));
    //temp.insert({adjusted_request->sessionid()*1000000+adjusted_request->label()*100+adjusted_request->kind(), constraint});
  }

  uint64_t parsing2 = getTimeStamp()-start;
  parsing2_total += parsing2;

  fut->finalize();
  return fut;
  //return nullptr;
}

static bool checkRequest(std::shared_ptr<JitCmdv2> cmd) {
  //per reset/solve pairs
  std::deque<std::deque<JitRequest*>> ReqList;
  std::deque<std::deque<JitRequest*>> res;
  std::deque<std::deque<JitRequest*>> single;

  for (int i = 0; i < cmd->expr_size(); i++) {
    single.clear();
    res.clear();
    JitRequest *expr = cmd->mutable_expr(i);
    //std::cout << "in parserequst and expr is " << std::endl;
    toDNF(expr, single);
    for(int i=0;i<ReqList.size();i++) {
      for(int j=0;j<single.size();j++) {
        std::deque<JitRequest*> cur;
        for(auto x : ReqList[i]) cur.push_back(x);
        for(auto y : single[j]) cur.push_back(y);
        res.push_back(cur);
      }
    }
    if (ReqList.size()==0) {
      for(int j=0;j<single.size();j++) {
        std::deque<JitRequest*> cur;
        for(auto y : single[j]) cur.push_back(y);
        res.push_back(cur);
      }
    }
    ReqList = res;
  }

  for (auto &subgoal : ReqList) {
    bool ret  = rejectTask(subgoal);
    if (ret)
      return false;
  }
  return true;
}


static void parseRequest(bool opti, std::shared_ptr<JitCmdv2> cmd, int threadId, std::deque<FUT*> &tasks,std::unordered_map<JitRequest, uint64_t, RequestHash, RequestEqual> &funcCache) {
  uint64_t start = getTimeStamp();
  //per reset/solve pairs
  std::deque<std::deque<JitRequest*>> ReqList;
  std::deque<std::deque<JitRequest*>> res;
  std::deque<std::deque<JitRequest*>> single;
  static int count = 0;
  count++;

  int n_expr = opti?1:cmd->expr_size();
  for (int i = 0; i < n_expr; i++) {
    single.clear();
    res.clear();
    JitRequest *expr = cmd->mutable_expr(i);
    //std::cout << "in parserequst and expr is " << std::endl;
    toDNF(expr, single);
    for(int i=0;i<ReqList.size();i++) {
      for(int j=0;j<single.size();j++) {
        std::deque<JitRequest*> cur;
        for(auto x : ReqList[i]) cur.push_back(x);
        for(auto y : single[j]) cur.push_back(y);
        res.push_back(cur);
      }
    }
    if (ReqList.size()==0) {
      for(int j=0;j<single.size();j++) {
        std::deque<JitRequest*> cur;
        for(auto y : single[j]) cur.push_back(y);
        res.push_back(cur);
      }
    }
    ReqList = res;
  }
  uint64_t parsing1 = getTimeStamp() - start;
  parsing1_total += parsing1;


  for (auto &subgoal : ReqList) {
    for (auto &subExpr : subgoal) {
      if (subExpr->kind() == rgd::Constant && 
          subExpr->value().compare("0") &&
          subgoal.size() ==1 ) {
        assert(false && "not sat expressions");
      }
    }
  }

  for (auto &subgoal : ReqList) {
    FUT *dut = constructTask(subgoal, threadId ,funcCache);
    //dumpFut(dut);
    if (dut)
      tasks.push_back(dut);
  }
}

static bool isExp(const JitRequest* req, std::unordered_set<uint32_t> &visited) {
  uint32_t kind = req->kind();
  if (kind == rgd::Shl && 
      req->children(0).kind() == rgd::Constant) {
    return true;
  }
  for (int i=0; i < req->children_size(); i++) {
    auto child = &req->children(i);
    uint32_t label = child->label();
    if (label!=0 && visited.count(label)==1)
      continue;
    visited.insert(label);
    if (isExp(child,visited))
      return true;
  }
  return false;
}


static bool screenCmd(std::shared_ptr<JitCmdv2> cmd) {
  //deprecating reset and solve
  assert(cmd->cmd()==2);
  uint64_t start = getTimeStamp();
  bool ret = checkRequest(cmd);
  uint64_t parsing = getTimeStamp() - start;
  parsing_total += parsing;
  return ret;
}


static void solveMemcmp(std::shared_ptr<JitCmdv2> cmd,
    std::unordered_map<uint32_t,uint8_t> &rgd_solution) {
  if (cmd->expr(0).children_size() == 1) {
    JitRequest c = cmd->expr(0).children(0);
    uint32_t index = c.index();
    std::string v = cmd->expr(0).value();
    std::cout << "v is " << v << " and bits is " << c.bits() << std::endl;
    for(int i=0;i<c.bits()/8;i++)
      rgd_solution[index+i] = v[i];
  }
  return;
}

// looking for simultanesouly existence of  LNot(expr) and expr
static bool checkContra(std::shared_ptr<JitCmdv2> cmd) {
  //label -> direction
  std::unordered_map<uint32_t, uint32_t> dir;
  for (int i = 0; i < cmd->expr_size(); i++) {
    if (cmd->expr(i).kind() == rgd::LNot) {
      auto itr = dir.find(cmd->expr(i).children(0).label());
      if (itr != dir.end()) {
        if (dir[cmd->expr(i).children(0).label()] == 1)
          return true;
      } else
        dir[cmd->expr(i).label()] = 0;
    } else {
      auto itr = dir.find(cmd->expr(i).label());
      if (itr != dir.end()) {
        if (dir[cmd->expr(i).label()] == 0)
          return true;
      } else
        dir[cmd->expr(i).label()] = 1;
    }
  }
  return false;
}

static int sendLocalCmd(bool opti, std::shared_ptr<JitCmdv2> cmd, int i,
    std::unordered_map<JitRequest, uint64_t, RequestHash, RequestEqual> &funcCache, 
    std::unordered_map<uint32_t,uint8_t> *rgd_solution, 
    std::unordered_map<uint32_t,uint8_t> *opti_solution, 
    std::unordered_map<uint32_t,uint8_t> *hint_solution, 
    std::string fname,
    uint64_t *st, uint64_t *iter) {
  assert (cmd->cmd() == 2); //deprecate "reset" and "solve"
  *st = 0;
  uint64_t start = getTimeStamp();
  std::deque<FUT*> tasks;
  if (checkContra(cmd)) return -1;
  try {
    parseRequest(opti, cmd, i, tasks,funcCache);
    uint64_t parsing = getTimeStamp() - start;
    parsing_total += parsing;

    uint64_t start_solving = getTimeStamp();
    for (auto dut : tasks) {
      dut->start = getTimeStamp();
      dut->rgd_solution = rgd_solution;
      dut->opti_solution = opti_solution;
      dut->hint_solution = hint_solution;
      //dut->load_buf(fname);
      dut->load_hint();
      bool suc = gd_entry(dut);
      uint64_t solving = getTimeStamp() - dut->start;
      solving_total += solving;
      iterations_total += dut->attempts;
      *iter  =  (*iter) + dut->attempts;
      *st = (*st) + getTimeStamp() - start_solving;
      if (suc) {
        break;
      }
    }
    for (auto dut : tasks) {
      //free(dut->scratch_args);
      delete dut;
    }
  } catch (...) {
    ++skipped;
    std::cerr << "something is wrong, continue" << std::endl;
    return -1;
  }

  return 0;
}



bool readDelimitedFrom(
    google::protobuf::io::ZeroCopyInputStream* rawInput,
    google::protobuf::MessageLite* message) {
  // We create a new coded stream for each message.  Don't worry, this is fast,
  // and it makes sure the 64MB total size limit is imposed per-message rather
  // than on the whole stream.  (See the CodedInputStream interface for more
  // info on this limit.)
  google::protobuf::io::CodedInputStream input(rawInput);

  // Read the size.
  uint32_t size;
  if (!input.ReadVarint32(&size)) return false;

  // Tell the stream not to read beyond that size.
  google::protobuf::io::CodedInputStream::Limit limit =
    input.PushLimit(size);

  // Parse the message.
  if (!message->MergeFromCodedStream(&input)) return false;
  if (!input.ConsumedEntireMessage()) return false;

  // Release the limit.
  input.PopLimit(limit);

  return true;
}


int gGenerate = 0;
//ctpl::thread_pool* gpool;
//ctpl::thread_pool* zpool;
std::vector<std::future<bool>> gresults;

std::vector<std::unordered_map<JitRequest, uint64_t, RequestHash, RequestEqual>> Expr2FuncList(64);
std::unordered_map<std::tuple<uint64_t,uint64_t,bool>, uint32_t, context_hash> BranchFilter;
std::vector<std::array<uint32_t,14>> IterHisList(64);
std::vector<std::array<uint32_t,14>> SucIterHisList(64);
std::vector<std::array<uint32_t,14>> ToIterHisList(64);
std::vector<std::shared_ptr<JitCmdv2>> allCmds;
std::unordered_map<uint32_t,std::shared_ptr<JitRequest>> nestedCache(10000);

struct thread_data {
  int thread_id;
  int amount;
};

moodycamel::ConcurrentQueue<std::shared_ptr<JitCmdv2>> jigsawqueue;



void* rgdTask(void *threadarg) {
  struct thread_data *my_data = (struct thread_data *) threadarg;
  int i = my_data->thread_id;
  int idx = 0;
  while ((idx = gCmdIdx.fetch_add(1, std::memory_order_relaxed)) < my_data->amount) {
    std::shared_ptr<JitCmdv2> cmd = allCmds[idx % my_data->amount];
#if NESTED_BRANCH
    std::shared_ptr<JitCmdv2> cmdDone = std::make_shared<JitCmdv2>();
    cmdDone->set_cmd(2);
    cmdDone->set_file_name(cmd->file_name());
    for(int i = 0 ;i < cmd->expr_size(); i++) {
      JitRequest* targetReq = cmdDone->add_expr();
      if (cmd->expr(i).direction() == 0) {
        targetReq->set_kind(rgd::LNot);
        targetReq->set_name("lnot");
        targetReq->set_bits(1);
        targetReq = targetReq->add_children();
      }
      if (cmd->expr(i).full() == 1) {
        targetReq->CopyFrom(cmd->expr(i));
      } else {
        struct nestKV *res = nestCache.find(cmd->expr(i).sessionid()*10000+cmd->expr(i).label());
        //auto req = nestCache[cmd->expr(i).sessionid()*10000+cmd->expr(i).label()];
        if (!res) {   //TODO exception here?
          continue;
        }
        targetReq->CopyFrom(*res->req);
      }
    }
    std::shared_ptr<JitCmdv2> cmdDoneOpt = std::make_shared<JitCmdv2>();
    cmdDoneOpt->set_cmd(2);
    JitRequest* targetReq = cmdDoneOpt->add_expr();
    targetReq->CopyFrom(cmdDone->expr(0));
#endif

#if VERIFY
    std::shared_ptr<JitCmdv2> verified_cmd = std::make_shared<JitCmdv2>();
    verified_cmd->CopyFrom(*cmdDone);
#endif

    uint64_t st = 0;
    uint64_t iter = 0;

    std::unordered_map<uint32_t,uint8_t> rgd_solution;
    std::unordered_map<uint32_t,uint8_t> opti_solution;
    std::unordered_map<uint32_t,uint8_t> hint_solution;

    // bool suc = sendZ3Solver(false,solvers[i],cmdDone, rgd_solution, &st);
    // if(rgd_solution.size()==0) return false;

    rgd_solution.clear();
    // search for the opt
    bool suc = false;
    uint64_t start = getTimeStamp();
#if NESTED_BRANCH
    if (cmdDoneOpt->expr(0).kind() != rgd::Memcmp) {
      int r = sendLocalCmd(true, cmdDoneOpt, i, Expr2FuncList[i],&rgd_solution, &opti_solution, &hint_solution, cmdDone->file_name(), &st, &iter);
    }

    if (rgd_solution.size() != 0)  {
      hint_solution = rgd_solution;
      iter = 0;
      rgd_solution.clear();
      if (cmdDone->expr(0).kind() != rgd::Memcmp) {
        int r = sendLocalCmd(false, cmdDone, i, Expr2FuncList[i],&rgd_solution, &opti_solution, &hint_solution, cmdDone->file_name(), &st, &iter);
      }
    } 
    //else {
    //   rgd_solution.clear();
    // }
#else
    if (cmd->expr(0).kind() != rgd::Memcmp) {
      int r = sendLocalCmd(false, cmd, i, Expr2FuncList[i],&rgd_solution, &opti_solution, &hint_solution, cmd->file_name(), &st, &iter);
    }
#endif

    uint64_t spent = getTimeStamp() - start;

    gProcessed++;

    if(rgd_solution.size()!=0) {
      gFi++;
    }
    if (gProcessed % 10000==0) {
      std::cout << "processed " << gProcessed << " constraints" << std::endl;
      std::cout << "elapsed time is " << getTimeStamp() - start_time <<  " cons cache lookup " << parsing_total3 << " iter " << iterations_total << " solved " << gFi << std::endl;
    }

#if CHECK_DIS
    if (rgd_solution.size()!=0) {
      FILE* fptr= fopen("jigsat.txt","a+");
      fprintf(fptr,"%lu\n", st);
      fclose(fptr);
      //fptr= fopen("jigsatiter.txt","a+");
      //fprintf(fptr,"%lu\n", iter);
      //fclose(fptr);
    } else if (iter>1000) {
      FILE* fptr= fopen("jigto.txt","a+");
      fprintf(fptr,"%lu\n", st);
      fclose(fptr);
      //fptr= fopen("jigtoiter.txt","a+");
      //fprintf(fptr,"%lu\n", iter);
      //fclose(fptr);
    }
    if (rgd_solution.size()!=0) {
      if (st<50) {
        SucIterHisList[i][0]++;
      } else if (st<100) {
        SucIterHisList[i][1]++;
      } else if (st<500) {
        SucIterHisList[i][2]++;
      } else if (st<1000) {
        SucIterHisList[i][3]++;
      } else if (st<5000) {
        SucIterHisList[i][4]++;
      } else if (st<10000) {
        SucIterHisList[i][5]++;
      } else if (st<50000) {
        SucIterHisList[i][6]++;
      } else if (st<100000) {
        SucIterHisList[i][7]++;
      } else if (st<1000000) {
        SucIterHisList[i][8]++;
      } else if (st<10000000) {
        SucIterHisList[i][9]++;
      } else {
        SucIterHisList[i][10]++;
      }
    } else if (iter<1000000) {
      if (st<50) {
        IterHisList[i][0]++;
      } else if (st<100) {
        IterHisList[i][1]++;
      } else if (st<500) {
        IterHisList[i][2]++;
      } else if (st<1000) {
        IterHisList[i][3]++;
      } else if (st<5000) {
        IterHisList[i][4]++;
      } else if (st<10000) {
        IterHisList[i][5]++;
      } else if (st<50000) {
        IterHisList[i][6]++;
      } else if (st<100000) {
        IterHisList[i][7]++;
      } else if (st<1000000) {
        IterHisList[i][8]++;
      } else if (st<10000000) {
        IterHisList[i][9]++;
      } else {
        IterHisList[i][10]++;
      }
    } else {
      if (st<50) {
        ToIterHisList[i][0]++;
      } else if (st<100) {
        ToIterHisList[i][1]++;
      } else if (st<500) {
        ToIterHisList[i][2]++;
      } else if (st<1000) {
        ToIterHisList[i][3]++;
      } else if (st<5000) {
        ToIterHisList[i][4]++;
      } else if (st<10000) {
        ToIterHisList[i][5]++;
      } else if (st<50000) {
        ToIterHisList[i][6]++;
      } else if (st<100000) {
        ToIterHisList[i][7]++;
      } else if (st<1000000) {
        ToIterHisList[i][8]++;
      } else if (st<10000000) {
        ToIterHisList[i][9]++;
      } else {
        ToIterHisList[i][10]++;
      }
    }
#endif
    }
  return nullptr;
}

void ReplayLocal(char** argv, int num_of_threads) {
  RGDServiceImpl s;
  int solved_count = 0;
  int i = 0;
  int allcount = 0;
  int dropcount = 0;
  int allexpr = 0;
  for (directory_entry& entry : directory_iterator(argv[3])) {
    int fd = open(entry.path().c_str(),O_RDONLY);
    //std::cout << "handling " << entry.path() << std::endl;
    ZeroCopyInputStream* rawInput = new google::protobuf::io::FileInputStream(fd);
    bool ret = false;
    //for each file
    do {
      std::shared_ptr<JitCmdv2> cmd = std::make_shared<JitCmdv2>();
      ret = readDelimitedFrom(rawInput,cmd.get());
      if (ret) {
        //caching the request
        allcount++;
        for (int i = 0 ; i< cmd->expr_string_size();i++) {
          JitRequest* req = cmd->add_expr();
          CodedInputStream s((uint8_t*)cmd->expr_string(i).c_str(), cmd->expr_string(i).size()); 
          s.SetRecursionLimit(100);
          //req->ParseFromString(cmd->expr_string(i));
          req->ParseFromCodedStream(&s);
        }
        for (int i = 0 ; i< cmd->expr_size();i++) {
          if (cmd->expr(i).full() == 1) {
            std::shared_ptr<JitRequest> cachedReq = std::make_shared<JitRequest>();
            cachedReq->CopyFrom(cmd->expr(i));
            //nestCache.insert({cmd->expr(i).sessionid()*10000+cmd->expr(i).label(), cachedReq});
            nestCache.insert(new struct nestKV(cmd->expr(i).sessionid()*10000+cmd->expr(i).label(), cachedReq));
          }
        }
        if (cmd->cmd() == 1) // not to solve
          continue;
      } else {
        break;
      }
      allexpr += cmd->expr_size();
      //building nested branches here consuming too much memory
#if SINGLE_BRANCH
      std::shared_ptr<JitCmdv2> cmdDone =  std::make_shared<JitCmdv2>();
      cmdDone->set_cmd(2);
      for(int i = 0 ;i < 1;i++) {
        JitRequest* targetReq = cmdDone->add_expr();
        if (cmd->expr(i).direction() == 0) {
          targetReq->set_kind(rgd::LNot);
          targetReq->set_name("lnot");
          targetReq->set_bits(1);
          targetReq = targetReq->add_children();
        }
        if (cmd->expr(i).full() == 1) {
          targetReq->CopyFrom(cmd->expr(i));
        }
      }
#endif
      // if (cmd->cmd()==2 && !drop) 
      if (cmd->cmd()==2) 
#if SINGLE_BRANCH
        allCmds.push_back(cmdDone);
#else
        allCmds.push_back(cmd);
#endif
    } while(ret);
    delete rawInput;
    close(fd);
    printf("\rLoading in progress %d", ++i);
    fflush(stdout);
    if (allCmds.size() > LOADING_LIMIT)
      break;
  }
  i = 0;
  printf("\nall count is %d, drop count is %d record number is %lu allexpr is %d\n",allcount,dropcount,allCmds.size(),allexpr);
  start_time = getTimeStamp();
  // ProfilerStart("pro.log");
  pthread_t threads[48];
  pthread_attr_t attr;
  struct thread_data td[48];
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  for(int k  = 0; k < num_of_threads ; k++ ) {
    td[k].thread_id = k;
    td[k].amount = allCmds.size();
  }
  cpu_set_t cpuset;
  for(int k  = 0; k < num_of_threads; k++ ) {
    pthread_create(&threads[k], &attr, rgdTask, (void *)&td[k]);
    CPU_ZERO(&cpuset);
    CPU_SET(k+core_start, &cpuset);
    pthread_setaffinity_np(threads[k], sizeof(cpu_set_t), &cpuset);
  }
  pthread_attr_destroy(&attr);
  for(int k = 0; k < num_of_threads; k++ ) {
    pthread_join(threads[k], NULL);
  }
  std::cout << "processing number is " << i << std::endl;
  std::cout << "elapsed time is " << getTimeStamp() - start_time << std::endl;
  std::cout << "missed sub expr is " << misssubexprs << std::endl;
  // ProfilerStop();
  std::cout << "sol " << gFi 
    << " ski " << skipped
    << " iter " << iterations_total
    << " sT " << solving_total
    << " pT " << parsing_total 
    << " parseMsgT  " << parsing1_total
    << " irT " 	<< parsing2_total 
    << " hit " << hit
    << " mis " << miss << std::endl;
}

int main(int argc, char** argv) {
  int num_of_threads = 0;
  int pin_core_start = 0;
  if (argc < 4) {
    std::cerr << "Use: ./rgd number_of_threads pin_core_start directory\n";
    std::cerr << "Example: ./rgd 32 0 0 test_dir\n";
    return 0;
  }
  if (sscanf (argv[1], "%i", &num_of_threads) != 1) {
    std::cerr << "error - not an integer, aborting...\n";
    return 0;
  }
  if (sscanf (argv[2], "%i", &pin_core_start) != 1) {
    std::cerr << "error - not an integer, aborting...\n";
    return 0;
  }
  std::cout << " number of threads " << num_of_threads << std::endl;
  std::cout << " pin_core_start " << pin_core_start << std::endl;
  core_start = pin_core_start;
  ReplayLocal(argv, num_of_threads);
  return 0;
}
