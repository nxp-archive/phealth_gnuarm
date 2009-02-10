/* { dg-do link { target i?86-*-linux* x86_64-*-linux* } } */
/* { dg-options "{-shared -fwhopr -msse2}" } */
/* { dg-suppress-ld-options {-msse2} } */

typedef short v8hi __attribute__((__vector_size__(16)));
void func (void) {
  v8hi x, y, z;
  z = __builtin_ia32_paddw128 (x, y);
}
