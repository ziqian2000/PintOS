#ifndef __LIB_FIXED_POINT_H
#define __LIB_FIXED_POINT_H

typedef int fixed_t;
#define FP_SHIFTING_BITS 16

#define FP_CONST_INT(A) ((fixed_t)((A) << FP_SHIFTING_BITS))

/* In the following FP_XXX_INT operation, always suppose A is a fixed-point while B is an integer. */

#define FP_ADD(A, B) ((A)+(B))
#define FP_ADD_INT(A, B) ((A) + FP_CONST_INT(B))

#define FP_SUB(A, B) ((A)-(B))
#define FP_SUB_INT(A, B) ((A) - FP_CONST_INT(B))

#define FP_MUL(A, B) ((fixed_t)((((int64_t)(A))*(B))>>FP_SHIFTING_BITS))
#define FP_MUL_INT(A, B) ((A)*(B))

#define FP_DIV(A, B) ((fixed_t)(((((int64_t)(A))<<FP_SHIFTING_BITS)/(B))))
#define FP_DIV_INT(A, B) ((A)/(B))

#define FP_ROUND_TOWARD_ZERO(A) ((A)>>FP_SHIFTING_BITS)
#define FP_ROUND_TO_NEAREST(A) ((A) >= 0 \
                                ? FP_ROUND_TOWARD_ZERO((A)+((1<<(FP_SHIFTING_BITS-1))))  \
                                : FP_ROUND_TOWARD_ZERO((A)-((1<<(FP_SHIFTING_BITS-1)))))

#endif /* lib/fixed_point.h */