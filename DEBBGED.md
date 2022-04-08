## AST encoding

Previsouly we only consider non-leaf parts of AST in the function cache. This becomes a problem when cache is used with tree-deduplication. 

Consider two expressions Sub(Add(A+B),A) and Sub(Add(A+B),B), where A, B are inputs. When the first query is processed, the JITTed function Sub(Add(arg0,arg1),arg0) will be saved in the cache. Because of tree-dedupliation, then second expression will be parsed as Sub(Add(arg0,arg1),arg1). But because only non-leaf are considered,  the JITed function saved for the first function will be used, resulting a function mismatch. 

In the new design, we traverse the tree using the post-order. We assigns index for each inputs and each constant and then include the indices for comparing the AST tree.  In this way, the first expression and second expression will be different functions in the cache.

##  New handling of relational expressions for better perf. in solving nested branches

In the context of nested branches, it is common to see the exactly opposite checks in the constraints. (e.g Ult(a,b) and Uge (a,b)). In the previsou design, Ult(a,b) and Uge(a,b) are compiled as different function, resulting a lot of cache miss. To solve this issue, we compile the same function for Ult(a,b) and Uge(a,b), which just outputs zext(a) and zext(b). And then we calculates the distance by invoking the JIT function.  

| Comparison                                                   | JITed function           |
|--------------------------------------------------------------|--------------------------|
| ult(a,b) ugt(a,b) ule(a,b) uge(a,b) equal(a,b) distance(a,b) | Outputs zext(a), zext(b) |
| slt(a,b) sgt(a,b) sle(a,b) sge(a,b)                          | Outputs sext(a), sext(b) |
