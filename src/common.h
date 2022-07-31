#ifndef MOCKIT_COMMON_H
#define MOCKIT_COMMON_H

/* 
 * Print to stdout iff flag is true */
void say__(int flag, char *fmt, ...);


#ifdef DEBUG_MODE
#   define say(...) say__ (__VA_ARGS__)
#else 
#   define say(...) ; /* empty statement => disable debug prints */
#endif


#endif /* MOCKIT_COMMON_H */
