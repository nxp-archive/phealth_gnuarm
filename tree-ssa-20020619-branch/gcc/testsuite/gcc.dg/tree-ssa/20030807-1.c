/* { dg-do compile */
/* { dg-options "-O1 -fdump-tree-ssa" } */
    
struct rtx_def;
typedef struct rtx_def *rtx;



union rtunion_def
{
  int rtint;
};
typedef union rtunion_def rtunion;



struct rtx_def
{
  rtunion fld[1];

};

static int *uid_cuid;
static int max_uid_cuid;

static void
bar ()
{

  rtx place = 0;

  if (place->fld[0].rtint <= max_uid_cuid
      && (place->fld[0].rtint > max_uid_cuid ? insn_cuid (place) :
	  uid_cuid[place->fld[0].rtint]))
    ;
}

/* There should be two IF conditionals.  */
/* { dg-final { scan-tree-dump-times "if " 2 "ssa"} } */
 

