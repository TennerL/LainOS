#ifndef LAINOS_FREESTANDING_SETJMP_H
#define LAINOS_FREESTANDING_SETJMP_H

typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value) __attribute__((noreturn));

int _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int value) __attribute__((noreturn));

#endif
