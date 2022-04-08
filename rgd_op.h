#ifndef __IR__H_
#define __IR__H_

namespace rgd {
	enum Kind {
		Bool, // 0
		Constant, // 1
		Read, // 2
		Concat, // 3
		Extract, // 4

		ZExt, // 5
		SExt, // 6

		// Arithmetic
		Add, // 7
		Sub, // 8
		Mul, // 9
		UDiv, // 10
		SDiv, // 11
		URem, // 12
		SRem, // 13
		Neg,  // 14

		// Bit
		Not, // 15
		And, // 16
		Or, // 17
		Xor, // 18
		Shl, // 19
		LShr, // 20
		AShr, // 21

		// Compare
		Equal, // 22
		Distinct, // 23
		Ult, // 24
		Ule, // 25
		Ugt, // 26
		Uge, // 27
		Slt, // 28
		Sle, // 29
		Sgt, // 30
		Sge, // 31

		// Logical
		LOr, // 32
		LAnd, // 33
		LNot, // 34

		// Special
		Ite, // 35
		Load, // 36    to be worked with TT-Fuzzer
		Memcmp, //37
	};
}

#endif
