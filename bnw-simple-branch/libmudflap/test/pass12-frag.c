int i = 10;
int *x = (void *)malloc(i * sizeof (int));

while (--i)
{
  ++x;
  *x = 0;
}
