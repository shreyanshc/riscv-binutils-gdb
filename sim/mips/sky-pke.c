/* Copyright (C) 1998, Cygnus Solutions */

/* Debugguing PKE? */
#define PKE_DEBUG 

#include <stdlib.h>
#include "sky-pke.h"
#include "sky-dma.h"
#include "sim-bits.h"
#include "sim-assert.h"
#include "sky-vu0.h"
#include "sky-vu1.h"
#include "sky-gpuif.h"


/* Imported functions */

void device_error (device *me, char* message);  /* device.c */


/* Internal function declarations */

static int pke_io_read_buffer(device*, void*, int, address_word,
			       unsigned, sim_cpu*, sim_cia);
static int pke_io_write_buffer(device*, const void*, int, address_word,
			       unsigned, sim_cpu*, sim_cia);
static void pke_issue(struct pke_device*);
static void pke_pc_advance(struct pke_device*, int num_words);
static unsigned_4* pke_pc_operand(struct pke_device*, int operand_num);
static unsigned_4 pke_pc_operand_bits(struct pke_device*, int bit_offset,
				      int bit_width, unsigned_4* sourceaddr);
static struct fifo_quadword* pke_pc_fifo(struct pke_device*, int operand_num, 
					 unsigned_4** operand);
static int pke_track_write(struct pke_device*, const void* src, int len,
			   address_word dest, unsigned_4 sourceaddr);
static void pke_attach(SIM_DESC sd, struct pke_device* me);
enum pke_check_target { chk_vu, chk_path1, chk_path2, chk_path3 };
static int pke_check_stall(struct pke_device* me, enum pke_check_target what);
static void pke_flip_dbf(struct pke_device* me);
/* PKEcode handlers */
static void pke_code_nop(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_stcycl(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_offset(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_base(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_itop(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_stmod(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_mskpath3(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_pkemark(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_flushe(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_flush(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_flusha(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_pkemscal(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_pkemscnt(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_pkemscalf(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_stmask(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_strow(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_stcol(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_mpg(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_direct(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_directhl(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_unpack(struct pke_device* me, unsigned_4 pkecode);
static void pke_code_error(struct pke_device* me, unsigned_4 pkecode);



/* Static data */

struct pke_device pke0_device = 
{ 
  { "pke0", &pke_io_read_buffer, &pke_io_write_buffer }, /* device */
  0, 0,        /* ID, flags */
  {},          /* regs */
  {}, 0,      /* FIFO write buffer */
  NULL, 0, 0, NULL,  /* FIFO */
  0, 0            /* pc */
};


struct pke_device pke1_device = 
{ 
  { "pke1", &pke_io_read_buffer, &pke_io_write_buffer }, /* device */
  1, 0,        /* ID, flags */
  {},          /* regs */
  {}, 0,       /* FIFO write buffer */
  NULL, 0, 0, NULL, /* FIFO */
  0, 0         /* pc */
};



/* External functions */


/* Attach PKE addresses to main memory */

void
pke0_attach(SIM_DESC sd) 
{
  pke_attach(sd, & pke0_device);
}

void
pke1_attach(SIM_DESC sd) 
{
  pke_attach(sd, & pke1_device);
}



/* Issue a PKE instruction if possible */

void 
pke0_issue(void) 
{
  pke_issue(& pke0_device);
}

void 
pke1_issue(void) 
{
  pke_issue(& pke0_device);
}



/* Internal functions */


/* Attach PKE memory regions to simulator */

void 
pke_attach(SIM_DESC sd, struct pke_device* me) 
{
  /* register file */
  sim_core_attach (sd,
		   NULL,
                   0 /*level*/,
                   access_read_write,
                   0 /*space ???*/,
		   (me->pke_number == 0) ? PKE0_REGISTER_WINDOW_START : PKE1_REGISTER_WINDOW_START,
                   PKE_REGISTER_WINDOW_SIZE /*nr_bytes*/,
                   0 /*modulo*/,
                   (device*) &pke0_device,
                   NULL /*buffer*/);

  /* FIFO port */
  sim_core_attach (sd,
		   NULL,
                   0 /*level*/,
                   access_read_write,
                   0 /*space ???*/,
		   (me->pke_number == 0) ? PKE0_FIFO_ADDR : PKE1_FIFO_ADDR,
                   sizeof(quadword) /*nr_bytes*/,
                   0 /*modulo*/,
                   (device*) &pke1_device,
                   NULL /*buffer*/);

  /* source-addr tracking word */
  sim_core_attach (sd,
		   NULL,
                   0 /*level*/,
                   access_read_write,
                   0 /*space ???*/,
		   (me->pke_number == 0) ? PKE0_SRCADDR : PKE1_SRCADDR,
                   sizeof(unsigned_4) /*nr_bytes*/,
                   0 /*modulo*/,
		   NULL, 
                   zalloc(sizeof(unsigned_4)) /*buffer*/);
}



/* Handle a PKE read; return no. of bytes read */

int
pke_io_read_buffer(device *me_,
		   void *dest,
		   int space,
		   address_word addr,
		   unsigned nr_bytes,
		   sim_cpu *cpu,
		   sim_cia cia)
{
  /* downcast to gather embedding pke_device struct */
  struct pke_device* me = (struct pke_device*) me_;

  /* find my address ranges */
  address_word my_reg_start =
    (me->pke_number == 0) ? PKE0_REGISTER_WINDOW_START : PKE1_REGISTER_WINDOW_START;
  address_word my_fifo_addr =
    (me->pke_number == 0) ? PKE0_FIFO_ADDR : PKE1_FIFO_ADDR;

  /* enforce that an access does not span more than one quadword */
  address_word low = ADDR_TRUNC_QW(addr);
  address_word high = ADDR_TRUNC_QW(addr + nr_bytes - 1);
  if(low != high)
    return 0;

  /* classify address & handle */
  if((addr >= my_reg_start) && (addr < my_reg_start + PKE_REGISTER_WINDOW_SIZE))
    {
      /* register bank */
      int reg_num = ADDR_TRUNC_QW(addr - my_reg_start) >> 4;
      int reg_byte = ADDR_OFFSET_QW(addr);      /* find byte-offset inside register bank */
      int readable = 1;
      quadword result;

      /* clear result */
      result[0] = result[1] = result[2] = result[3] = 0;

      /* handle reads to individual registers; clear `readable' on error */
      switch(reg_num)
	{
	  /* handle common case of register reading, side-effect free */
	  /* PKE1-only registers*/
	case PKE_REG_BASE:
	case PKE_REG_OFST:
	case PKE_REG_TOPS:
	case PKE_REG_TOP:
	case PKE_REG_DBF:
	  if(me->pke_number == 0)
	    readable = 0;
	  /* fall through */
	  /* PKE0 & PKE1 common registers*/
	case PKE_REG_STAT:
	case PKE_REG_ERR:
	case PKE_REG_MARK:
	case PKE_REG_CYCLE:
	case PKE_REG_MODE:
	case PKE_REG_NUM:
	case PKE_REG_MASK:
	case PKE_REG_CODE:
	case PKE_REG_ITOPS:
	case PKE_REG_ITOP:
	case PKE_REG_R0:
	case PKE_REG_R1:
	case PKE_REG_R2:
	case PKE_REG_R3:
	case PKE_REG_C0:
	case PKE_REG_C1:
	case PKE_REG_C2:
	case PKE_REG_C3:
	  result[0] = me->regs[reg_num][0];
	  break;

	  /* handle common case of write-only registers */
	case PKE_REG_FBRST:
	  readable = 0;
	  break;

	default:
	  ASSERT(0); /* test above should prevent this possibility */
	}

      /* perform transfer & return */
      if(readable) 
	{
	  /* copy the bits */
	  memcpy(dest, ((unsigned_1*) &result) + reg_byte, nr_bytes);
	  /* okay */
	  return nr_bytes;
	}
      else
	{
	  /* error */
	  return 0;
	} 

      /* NOTREACHED */
    }
  else if(addr >= my_fifo_addr &&
	  addr < my_fifo_addr + sizeof(quadword))
    {
      /* FIFO */

      /* FIFO is not readable: return a word of zeroes */
      memset(dest, 0, nr_bytes);
      return nr_bytes;
    }

  /* NOTREACHED */
  return 0;
}


/* Handle a PKE read; return no. of bytes written */

int
pke_io_write_buffer(device *me_,
		    const void *src,
		    int space,
		    address_word addr,
		    unsigned nr_bytes,
		    sim_cpu *cpu,
		    sim_cia cia)
{ 
  /* downcast to gather embedding pke_device struct */
  struct pke_device* me = (struct pke_device*) me_;

  /* find my address ranges */
  address_word my_reg_start =
    (me->pke_number == 0) ? PKE0_REGISTER_WINDOW_START : PKE1_REGISTER_WINDOW_START;
  address_word my_fifo_addr =
    (me->pke_number == 0) ? PKE0_FIFO_ADDR : PKE1_FIFO_ADDR;

  /* enforce that an access does not span more than one quadword */
  address_word low = ADDR_TRUNC_QW(addr);
  address_word high = ADDR_TRUNC_QW(addr + nr_bytes - 1);
  if(low != high)
    return 0;

  /* classify address & handle */
  if((addr >= my_reg_start) && (addr < my_reg_start + PKE_REGISTER_WINDOW_SIZE))
    {
      /* register bank */
      int reg_num = ADDR_TRUNC_QW(addr - my_reg_start) >> 4;
      int reg_byte = ADDR_OFFSET_QW(addr);      /* find byte-offset inside register bank */
      int writeable = 1;
      quadword input;

      /* clear input */
      input[0] = input[1] = input[2] = input[3] = 0;

      /* write user-given bytes into input */
      memcpy(((unsigned_1*) &input) + reg_byte, src, nr_bytes);

      /* handle writes to individual registers; clear `writeable' on error */
      switch(reg_num)
	{
	case PKE_REG_FBRST:
	  /* Order these tests from least to most overriding, in case
             multiple bits are set. */
	  if(BIT_MASK_GET(input[0], 2, 2)) /* STC bit */
	    {
	      /* clear a bunch of status bits */
	      PKE_REG_MASK_SET(me, STAT, PSS, 0);
	      PKE_REG_MASK_SET(me, STAT, PFS, 0);
	      PKE_REG_MASK_SET(me, STAT, PIS, 0);
	      PKE_REG_MASK_SET(me, STAT, INT, 0);
	      PKE_REG_MASK_SET(me, STAT, ER0, 0);
	      PKE_REG_MASK_SET(me, STAT, ER1, 0);
	      me->flags &= ~PKE_FLAG_PENDING_PSS;
	      /* will allow resumption of possible stalled instruction */
	    }
	  if(BIT_MASK_GET(input[0], 2, 2)) /* STP bit */
	    {
	      me->flags |= PKE_FLAG_PENDING_PSS;
	    }
	  if(BIT_MASK_GET(input[0], 1, 1)) /* FBK bit */
	    {
	      PKE_REG_MASK_SET(me, STAT, PFS, 1);
	    }
	  if(BIT_MASK_GET(input[0], 0, 0)) /* RST bit */
	    {
	      /* clear FIFO by skipping to word after PC: also
                 prevents re-execution attempt of possible stalled
                 instruction */
	      me->fifo_num_elements = me->fifo_pc;
	      /* clear registers, flag, other state */
	      memset(me->regs, 0, sizeof(me->regs));
	      me->fifo_qw_done = 0;
	      me->flags = 0;
	      me->qw_pc = 0;
	    }
	  break;

	case PKE_REG_ERR:
	  /* copy bottom three bits */
	  BIT_MASK_SET(me->regs[PKE_REG_ERR][0], 0, 2, BIT_MASK_GET(input[0], 0, 2));
	  break;

	case PKE_REG_MARK:
	  /* copy bottom sixteen bits */
	  PKE_REG_MASK_SET(me, MARK, MARK, BIT_MASK_GET(input[0], 0, 15));
	  /* reset MRK bit in STAT */
	  PKE_REG_MASK_SET(me, STAT, MRK, 0);
	  break;

	  /* handle common case of read-only registers */
	  /* PKE1-only registers - not really necessary to handle separately */
	case PKE_REG_BASE:
	case PKE_REG_OFST:
	case PKE_REG_TOPS:
	case PKE_REG_TOP:
	case PKE_REG_DBF:
	  if(me->pke_number == 0)
	    writeable = 0;
	  /* fall through */
	  /* PKE0 & PKE1 common registers*/
	case PKE_REG_STAT:
	  /* ignore FDR bit for PKE1_STAT -- simulator does not implement PKE->RAM transfers */
	case PKE_REG_CYCLE:
	case PKE_REG_MODE:
	case PKE_REG_NUM:
	case PKE_REG_MASK:
	case PKE_REG_CODE:
	case PKE_REG_ITOPS:
	case PKE_REG_ITOP:
	case PKE_REG_R0:
	case PKE_REG_R1:
	case PKE_REG_R2:
	case PKE_REG_R3:
	case PKE_REG_C0:
	case PKE_REG_C1:
	case PKE_REG_C2:
	case PKE_REG_C3:
	  writeable = 0;
	  break;

	default:
	  ASSERT(0); /* test above should prevent this possibility */
	}

      /* perform return */
      if(writeable) 
	{
	  /* okay */
	  return nr_bytes;
	}
      else
	{
	  /* error */
	  return 0;
	} 

      /* NOTREACHED */
    }
  else if(addr >= my_fifo_addr &&
	  addr < my_fifo_addr + sizeof(quadword))
    {
      /* FIFO */
      struct fifo_quadword* fqw;
      int fifo_byte = ADDR_OFFSET_QW(addr);      /* find byte-offset inside fifo quadword */
      int i;

      /* collect potentially-partial quadword in write buffer */
      memcpy(((unsigned_1*)& me->fifo_qw_in_progress) + fifo_byte, src, nr_bytes);
      /* mark bytes written */
      for(i = fifo_byte; i < fifo_byte + nr_bytes; i++)
	BIT_MASK_SET(me->fifo_qw_done, i, i, 1);

      /* return if quadword not quite written yet */
      if(BIT_MASK_GET(me->fifo_qw_done, 0, sizeof(quadword)-1) !=
	 BIT_MASK_BTW(0, sizeof(quadword)))
	return nr_bytes;

      /* all done - process quadword after clearing flag */
      BIT_MASK_SET(me->fifo_qw_done, 0, sizeof(quadword)-1, 0);

      /* ensure FIFO has enough elements */
      if(me->fifo_num_elements == me->fifo_buffer_size)
	{
	  /* time to grow */
	  int new_fifo_buffer_size = me->fifo_buffer_size + 20;
	  void* ptr = realloc((void*) me->fifo, new_fifo_buffer_size*sizeof(quadword));

	  if(ptr == NULL)
	    {
	      /* oops, cannot enlarge FIFO any more */
	      device_error(me_, "Cannot enlarge FIFO buffer\n");
	      return 0;
	    }

	  me->fifo_buffer_size = new_fifo_buffer_size;
	}

      /* add new quadword at end of FIFO */
      fqw = & me->fifo[me->fifo_num_elements];
      memcpy((void*) fqw->data, me->fifo_qw_in_progress, sizeof(quadword));
      sim_read(CPU_STATE(cpu),
	       (SIM_ADDR) (me->pke_number == 0 ? DMA_D0_MADR : DMA_D1_MADR),
	       (void*) & fqw->source_address,
	       sizeof(address_word));
      sim_read(CPU_STATE(cpu),
	       (SIM_ADDR) (me->pke_number == 0 ? DMA_D0_PKTFLAG : DMA_D1_PKTFLAG),
	       (void*) & fqw->dma_tag_present,
	       sizeof(unsigned_4));

      me->fifo_num_elements++;

      /* set FQC to "1" as FIFO is now not empty */ 
      PKE_REG_MASK_SET(me, STAT, FQC, 1);
      
      /* okay */
      return nr_bytes;
    }

  /* NOTREACHED */
  return 0;
}



/* Issue & swallow next PKE opcode if possible/available */

void
pke_issue(struct pke_device* me)
{
  struct fifo_quadword* fqw;
  unsigned_4 fw;
  unsigned_4 cmd, intr, num;
  unsigned_4 imm;

  /* 1 -- test go / no-go for PKE execution */

  /* switch on STAT:PSS if PSS-pending and in idle state */
  if((PKE_REG_MASK_GET(me, STAT, PPS) == PKE_REG_STAT_PPS_IDLE) &&
     (me->flags & PKE_FLAG_PENDING_PSS) != 0)
    {
      me->flags &= ~PKE_FLAG_PENDING_PSS;
      PKE_REG_MASK_SET(me, STAT, PSS, 1);
    }

  /* check for stall/halt control bits */
  if(PKE_REG_MASK_GET(me, STAT, PFS) ||
     PKE_REG_MASK_GET(me, STAT, PSS) || /* note special treatment below */
     /* PEW bit not a reason to keep stalling - it's re-checked below */
     /* PGW bit not a reason to keep stalling - it's re-checked below */
     /* maskable stall controls: ER0, ER1, PIS */
     (PKE_REG_MASK_GET(me, STAT, ER0) && !PKE_REG_MASK_GET(me, ERR, ME0)) ||
     (PKE_REG_MASK_GET(me, STAT, ER1) && !PKE_REG_MASK_GET(me, ERR, ME1)) ||
     (PKE_REG_MASK_GET(me, STAT, PIS) && !PKE_REG_MASK_GET(me, ERR, MII)))
    {
      /* try again next cycle; no state change */
      return;
    }

  /* confirm availability of new quadword of PKE instructions */
  if(me->fifo_num_elements <= me->fifo_pc)
    return;


  /* 2 -- fetch PKE instruction */

  /* skip over DMA tag, if present */
  pke_pc_advance(me, 0);

  /* "fetch" instruction quadword and word */ 
  fqw = & me->fifo[me->fifo_pc];
  fw = fqw->data[me->qw_pc];

  /* store word in PKECODE register */
  me->regs[PKE_REG_CODE][0] = fw;


  /* 3 -- decode PKE instruction */

  /* PKE instruction format: [intr 0:0][pke-command 6:0][num 7:0][immediate 15:0],
     so op-code is in top byte. */
  intr = BIT_MASK_GET(fw, PKE_OPCODE_I_B,   PKE_OPCODE_I_E);
  cmd  = BIT_MASK_GET(fw, PKE_OPCODE_CMD_B, PKE_OPCODE_CMD_E);
  num  = BIT_MASK_GET(fw, PKE_OPCODE_NUM_B, PKE_OPCODE_NUM_E);
  imm  = BIT_MASK_GET(fw, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);

  if(intr)
    {
      /* set INT flag in STAT register */
      PKE_REG_MASK_SET(me, STAT, INT, 1);
      /* XXX: how to send interrupt to R5900? */
    }

  /* decoding */
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_DECODE);

  /* decode & execute */
  if(IS_PKE_CMD(cmd, PKENOP))
    pke_code_nop(me, fw);
  else if(IS_PKE_CMD(cmd, STCYCL))
    pke_code_stcycl(me, fw);
  else if(me->pke_number == 1 && IS_PKE_CMD(cmd, OFFSET))
    pke_code_offset(me, fw);
  else if(me->pke_number == 1 && IS_PKE_CMD(cmd, BASE))
    pke_code_base(me, fw);
  else if(IS_PKE_CMD(cmd, ITOP))
    pke_code_itop(me, fw);
  else if(IS_PKE_CMD(cmd, STMOD))
    pke_code_stmod(me, fw);
  else if(me->pke_number == 1 && IS_PKE_CMD(cmd, MSKPATH3))
    pke_code_mskpath3(me, fw);
  else if(IS_PKE_CMD(cmd, PKEMARK))
    pke_code_pkemark(me, fw);
  else if(IS_PKE_CMD(cmd, FLUSHE))
    pke_code_flushe(me, fw);
  else if(me->pke_number == 1 && IS_PKE_CMD(cmd, FLUSH))
    pke_code_flush(me, fw);
  else if(me->pke_number == 1 && IS_PKE_CMD(cmd, FLUSHA))
    pke_code_flusha(me, fw);
  else if(IS_PKE_CMD(cmd, PKEMSCAL))
    pke_code_pkemscal(me, fw);
  else if(IS_PKE_CMD(cmd, PKEMSCNT))
    pke_code_pkemscnt(me, fw);
  else if(me->pke_number == 1 && IS_PKE_CMD(cmd, PKEMSCALF))
    pke_code_pkemscalf(me, fw);
  else if(IS_PKE_CMD(cmd, STMASK))
    pke_code_stmask(me, fw);
  else if(IS_PKE_CMD(cmd, STROW))
    pke_code_strow(me, fw);
  else if(IS_PKE_CMD(cmd, STCOL))
    pke_code_stcol(me, fw);
  else if(IS_PKE_CMD(cmd, MPG))
    pke_code_mpg(me, fw);
  else if(IS_PKE_CMD(cmd, DIRECT))
    pke_code_direct(me, fw);
  else if(IS_PKE_CMD(cmd, DIRECTHL))
    pke_code_directhl(me, fw);
  else if(IS_PKE_CMD(cmd, UNPACK))
    pke_code_unpack(me, fw);
  /* ... no other commands ... */
  else
    pke_code_error(me, fw);
}



/* advance the PC by given number of data words; update STAT/FQC
   field; assume FIFO is filled enough */

void
pke_pc_advance(struct pke_device* me, int num_words)
{
  int num = num_words;
  ASSERT(num_words > 0);

  while(num > 0)
    {
      struct fifo_quadword* fq;

      /* one word skipped */
      num --;

      /* point to next word */
      me->qw_pc ++;
      if(me->qw_pc == 4)
	{
	  me->qw_pc = 0;
	  me->fifo_pc ++;
	}

      /* skip over DMA tag words if present in word 0 or 1 */
      fq = & me->fifo[me->fifo_pc];
      if(fq->dma_tag_present && (me->qw_pc < 2))
	{
	  /* skip by going around loop an extra time */
	  num ++;
	}
    }

  /* clear FQC if FIFO is now empty */ 
  if(me->fifo_num_elements == me->fifo_pc)
    {
      PKE_REG_MASK_SET(me, STAT, FQC, 0);
    }
}



/* Return pointer to FIFO quadword containing given operand# in FIFO.
   `operand_num' starts at 1.  Return pointer to operand word in last
   argument, if non-NULL.  If FIFO is not full enough, return 0.
   Signal an ER0 indication upon skipping a DMA tag.  */

struct fifo_quadword*
pke_pc_fifo(struct pke_device* me, int operand_num, unsigned_4** operand)
{
  int num = operand_num;
  int new_qw_pc, new_fifo_pc;
  struct fifo_quadword* operand_fifo = NULL;

  ASSERT(num > 0);

  /* snapshot current pointers */
  new_fifo_pc = me->fifo_pc;
  new_qw_pc = me->qw_pc;

  while(num > 0)
    {
      /* one word skipped */
      num --;

      /* point to next word */
      new_qw_pc ++;
      if(new_qw_pc == 4)
	{
	  new_qw_pc = 0;
	  new_fifo_pc ++;
	}

      /* check for FIFO underflow */
      if(me->fifo_num_elements == new_fifo_pc)
	{
	  operand_fifo = NULL;
	  break;
	}

      /* skip over DMA tag words if present in word 0 or 1 */
      operand_fifo = & me->fifo[new_fifo_pc];
      if(operand_fifo->dma_tag_present && (new_qw_pc < 2))
	{
	  /* mismatch error! */
	  PKE_REG_MASK_SET(me, STAT, ER0, 1);
	  /* skip by going around loop an extra time */
	  num ++;
	}
    }

  /* return pointer to operand word itself */
  if(operand_fifo != NULL)
    *operand = & operand_fifo->data[new_qw_pc];

  return operand_fifo;
}


/* Return pointer to given operand# in FIFO.  `operand_num' starts at 1.
   If FIFO is not full enough, return 0.  Skip over DMA tags, but mark
   them as an error (ER0). */

unsigned_4*
pke_pc_operand(struct pke_device* me, int operand_num)
{
  unsigned_4* operand = NULL;
  struct fifo_quadword* fifo_operand;

  fifo_operand = pke_pc_fifo(me, operand_num, & operand);

  if(fifo_operand == NULL)
    ASSERT(operand == NULL); /* pke_pc_fifo() ought leave it untouched */

  return operand;
}


/* Return a bit-field extract of given operand# in FIFO, and its
   source-addr.  `bit_offset' starts at 0, referring to LSB after PKE
   instruction word.  Width must be >0, <=32.  Assume FIFO is full
   enough.  Skip over DMA tags, but mark them as an error (ER0). */

unsigned_4
pke_pc_operand_bits(struct pke_device* me, int bit_offset, int bit_width, unsigned_4* source_addr)
{
  unsigned_4* word = NULL;
  unsigned_4 value;
  struct fifo_quadword* fifo_operand;

  /* find operand word with bitfield */
  fifo_operand = pke_pc_fifo(me, (bit_offset / 32) + 1, &word);
  ASSERT(word != 0);

  /* extract bitfield from word */
  value = BIT_MASK_GET(*word, bit_offset % 32, bit_width);

  /* extract source addr from fifo word */
  *source_addr = fifo_operand->source_address;

  return value;
}





/* Write a bunch of bytes into simulator memory.  Store the given source address into the
   PKE sourceaddr tracking word. */
int
pke_track_write(struct pke_device* me, const void* src, int len, 
		address_word dest, unsigned_4 sourceaddr)
{
  int rc;
  unsigned_4 no_sourceaddr = 0;

  /* write srcaddr into PKE srcaddr tracking */
  sim_write(NULL,
	    (SIM_ADDR) (me->pke_number == 0) ? PKE0_SRCADDR : PKE1_SRCADDR,
	    (void*) & sourceaddr,
	    sizeof(unsigned_4));
  
  /* write bytes into simulator */
  rc = sim_write(NULL,
		 (SIM_ADDR) dest,
		 (void*) src,
		 len);
  
  /* clear srcaddr from PKE srcaddr tracking */
  sim_write(NULL,
	    (SIM_ADDR) (me->pke_number == 0) ? PKE0_SRCADDR : PKE1_SRCADDR,
	    (void*) & no_sourceaddr,
	    sizeof(unsigned_4));

  return rc;
}


/* check for stall conditions on indicated devices (path* only on PKE1), do not change status
   return 0 iff no stall */ 
int
pke_check_stall(struct pke_device* me, enum pke_check_target what)
{
  int any_stall = 0;

  /* read GPUIF status word - commonly used */
  unsigned_4 gpuif_stat;
  sim_read(NULL,
	   (SIM_ADDR) (GIF_REG_STAT),
	   (void*) & gpuif_stat,
	   sizeof(unsigned_4));

  /* perform checks */
  if(what == chk_vu)
    {
      ASSERT(0);
      /* XXX: have to check COP2 control register VBS0 / VBS1 bits */
    }
  else if(what == chk_path1) /* VU -> GPUIF */
    {
      if(BIT_MASK_GET(gpuif_stat, GPUIF_REG_STAT_APATH_B, GPUIF_REG_STAT_APATH_E) == 1)
	any_stall = 1;
    }
  else if(what == chk_path2) /* PKE -> GPUIF */
    {
      if(BIT_MASK_GET(gpuif_stat, GPUIF_REG_STAT_APATH_B, GPUIF_REG_STAT_APATH_E) == 2)
	any_stall = 1;
    }
  else if(what == chk_path3) /* DMA -> GPUIF */
    {
      if(BIT_MASK_GET(gpuif_stat, GPUIF_REG_STAT_APATH_B, GPUIF_REG_STAT_APATH_E) == 3)
	any_stall = 1;
    }
  else
    {
      /* invalid what */
      ASSERT(0);
    }

  /* any stall reasons? */
  return any_stall;
}


/* flip the DBF bit; recompute TOPS, ITOP & TOP */
void
pke_flip_dbf(struct pke_device* me)
{
  /* flip DBF */
  PKE_REG_MASK_SET(me, DBF, DF,
		   PKE_REG_MASK_GET(me, DBF, DF) ? 0 : 1);
  PKE_REG_MASK_SET(me, STAT, DBF, PKE_REG_MASK_GET(me, DBF, DF));
  /* compute new TOPS */
  PKE_REG_MASK_SET(me, TOPS, TOPS,
		   (PKE_REG_MASK_GET(me, BASE, BASE) +
		    (PKE_REG_MASK_GET(me, DBF, DF) *
		     PKE_REG_MASK_GET(me, OFST, OFFSET))));
  /* compute new ITOP and TOP */
  PKE_REG_MASK_SET(me, ITOP, ITOP,
		   PKE_REG_MASK_GET(me, ITOPS, ITOPS));
  PKE_REG_MASK_SET(me, TOP, TOP,
		   PKE_REG_MASK_GET(me, TOPS, TOPS));
}



/* PKEcode handler functions -- responsible for checking and
   confirming old stall conditions, executing pkecode, updating PC and
   status registers -- may assume being run on correct PKE unit */
   
void 
pke_code_nop(struct pke_device* me, unsigned_4 pkecode)
{
  /* done */
  pke_pc_advance(me, 1);
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
}


void
pke_code_stcycl(struct pke_device* me, unsigned_4 pkecode)
{
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
  /* copy immediate value into CYCLE reg */
  me->regs[PKE_REG_CYCLE][0] = imm;
  /* done */
  pke_pc_advance(me, 1);
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
}


void
pke_code_offset(struct pke_device* me, unsigned_4 pkecode)
{
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
  /* copy 10 bits to OFFSET field */
  PKE_REG_MASK_SET(me, OFST, OFFSET, BIT_MASK_GET(imm, 0, 9));
  /* clear DBF bit */
  PKE_REG_MASK_SET(me, DBF, DF, 0);
  /* clear other DBF bit */
  PKE_REG_MASK_SET(me, STAT, DBF, 0);
  /* set TOPS = BASE */
  PKE_REG_MASK_SET(me, TOPS, TOPS, PKE_REG_MASK_GET(me, BASE, BASE));
  /* done */
  pke_pc_advance(me, 1);
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
}


void
pke_code_base(struct pke_device* me, unsigned_4 pkecode)
{
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
  /* copy 10 bits to BASE field */
  PKE_REG_MASK_SET(me, BASE, BASE, BIT_MASK_GET(imm, 0, 9));
  /* clear DBF bit */
  PKE_REG_MASK_SET(me, DBF, DF, 0);
  /* clear other DBF bit */
  PKE_REG_MASK_SET(me, STAT, DBF, 0);
  /* set TOPS = BASE */
  PKE_REG_MASK_SET(me, TOPS, TOPS, PKE_REG_MASK_GET(me, BASE, BASE));
  /* done */
  pke_pc_advance(me, 1);
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
}


void
pke_code_itop(struct pke_device* me, unsigned_4 pkecode)
{
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
  /* copy 10 bits to ITOPS field */
  PKE_REG_MASK_SET(me, ITOPS, ITOPS, BIT_MASK_GET(imm, 0, 9));
  /* done */
  pke_pc_advance(me, 1);
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
}


void
pke_code_stmod(struct pke_device* me, unsigned_4 pkecode)
{
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
  /* copy 2 bits to MODE register */
  PKE_REG_MASK_SET(me, MODE, MDE, BIT_MASK_GET(imm, 0, 2));
  /* done */
  pke_pc_advance(me, 1);
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
}


void
pke_code_mskpath3(struct pke_device* me, unsigned_4 pkecode)
{
  ASSERT(0);
  /* XXX: no easy interface toward GPUIF for this purpose */
}


void
pke_code_pkemark(struct pke_device* me, unsigned_4 pkecode)
{
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
  /* copy 16 bits to MARK register */
  PKE_REG_MASK_SET(me, MARK, MARK, BIT_MASK_GET(imm, 0, 15));
  /* set MRK bit in STAT register - CPU2 v2.1 docs incorrect */
  PKE_REG_MASK_SET(me, STAT, MRK, 1);
  /* done */
  pke_pc_advance(me, 1);
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
}


void
pke_code_flushe(struct pke_device* me, unsigned_4 pkecode)
{
  /* compute next PEW bit */
  if(pke_check_stall(me, chk_vu))
    {
      /* VU busy */
      PKE_REG_MASK_SET(me, STAT, PEW, 1);
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_STALL);
      /* try again next cycle */
    }
  else
    {
      /* VU idle */
      PKE_REG_MASK_SET(me, STAT, PEW, 0);
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 1);
    }
}


void
pke_code_flush(struct pke_device* me, unsigned_4 pkecode)
{
  int something_busy = 0;

  /* compute next PEW, PGW bits */
  if(pke_check_stall(me, chk_vu))
    {
      something_busy = 1;
      PKE_REG_MASK_SET(me, STAT, PEW, 1);
    }
  else
    PKE_REG_MASK_SET(me, STAT, PEW, 0);


  if(pke_check_stall(me, chk_path1) ||
     pke_check_stall(me, chk_path2))
    {
      something_busy = 1;
      PKE_REG_MASK_SET(me, STAT, PGW, 1);
    }
  else
    PKE_REG_MASK_SET(me, STAT, PGW, 0);

  /* go or no go */
  if(something_busy)
    {
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* try again next cycle */
    }
  else
    {
      /* all idle */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 1);
    }
}


void
pke_code_flusha(struct pke_device* me, unsigned_4 pkecode)
{
  int something_busy = 0;

  /* compute next PEW, PGW bits */
  if(pke_check_stall(me, chk_vu))
    {
      something_busy = 1;
      PKE_REG_MASK_SET(me, STAT, PEW, 1);
    }
  else
    PKE_REG_MASK_SET(me, STAT, PEW, 0);


  if(pke_check_stall(me, chk_path1) ||
     pke_check_stall(me, chk_path2) ||
     pke_check_stall(me, chk_path3))
    {
      something_busy = 1;
      PKE_REG_MASK_SET(me, STAT, PGW, 1);
    }
  else
    PKE_REG_MASK_SET(me, STAT, PGW, 0);

  if(something_busy)
    {
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* try again next cycle */
    }
  else
    {
      /* all idle */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 1);
    }
}


void
pke_code_pkemscal(struct pke_device* me, unsigned_4 pkecode)
{
  /* compute next PEW bit */
  if(pke_check_stall(me, chk_vu))
    {
      /* VU busy */
      PKE_REG_MASK_SET(me, STAT, PEW, 1);
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_STALL);
      /* try again next cycle */
    }
  else
    {
      unsigned_4 vu_pc;
      int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);

      /* VU idle */
      PKE_REG_MASK_SET(me, STAT, PEW, 0);

      /* flip DBF on PKE1 */
      if(me->pke_number == 1)
	pke_flip_dbf(me);

      /* compute new PC for VU */
      vu_pc = BIT_MASK_GET(imm, 0, 15);
      /* write new PC; callback function gets VU running */
      sim_write(NULL,
		(SIM_ADDR) (me->pke_number == 0 ? VU0_PC_START : VU1_PC_START),
		(void*) & vu_pc,
		sizeof(unsigned_4));

      /* done */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 1);
    }
}



void
pke_code_pkemscnt(struct pke_device* me, unsigned_4 pkecode)
{
  /* compute next PEW bit */
  if(pke_check_stall(me, chk_vu))
    {
      /* VU busy */
      PKE_REG_MASK_SET(me, STAT, PEW, 1);
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_STALL);
      /* try again next cycle */
    }
  else
    {
      unsigned_4 vu_pc;

      /* VU idle */
      PKE_REG_MASK_SET(me, STAT, PEW, 0);

      /* flip DBF on PKE1 */
      if(me->pke_number == 1)
	pke_flip_dbf(me);

      /* read old PC */
      sim_read(NULL,
	       (SIM_ADDR) (me->pke_number == 0 ? VU0_PC_START : VU1_PC_START),
	       (void*) & vu_pc,
	       sizeof(unsigned_4));

      /* rewrite new PC; callback function gets VU running */
      sim_write(NULL,
		(SIM_ADDR) (me->pke_number == 0 ? VU0_PC_START : VU1_PC_START),
		(void*) & vu_pc,
		sizeof(unsigned_4));

      /* done */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 1);
    }
}


void
pke_code_pkemscalf(struct pke_device* me, unsigned_4 pkecode)
{
  int something_busy = 0;

  /* compute next PEW, PGW bits */
  if(pke_check_stall(me, chk_vu))
    {
      something_busy = 1;
      PKE_REG_MASK_SET(me, STAT, PEW, 1);
    }
  else
    PKE_REG_MASK_SET(me, STAT, PEW, 0);


  if(pke_check_stall(me, chk_path1) ||
     pke_check_stall(me, chk_path2) ||
     pke_check_stall(me, chk_path3))
    {
      something_busy = 1;
      PKE_REG_MASK_SET(me, STAT, PGW, 1);
    }
  else
    PKE_REG_MASK_SET(me, STAT, PGW, 0);

  /* go or no go */
  if(something_busy)
    {
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* try again next cycle */
    }
  else
    {
      unsigned_4 vu_pc;
      int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
      
      /* flip DBF on PKE1 */
      if(me->pke_number == 1)
	pke_flip_dbf(me);

      /* compute new PC for VU */
      vu_pc = BIT_MASK_GET(imm, 0, 15);
      /* write new PC; callback function gets VU running */
      sim_write(NULL,
		(SIM_ADDR) (me->pke_number == 0 ? VU0_PC_START : VU1_PC_START),
		(void*) & vu_pc,
		sizeof(unsigned_4));

      /* done */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 1);
    }
}


void
pke_code_stmask(struct pke_device* me, unsigned_4 pkecode)
{
  /* check that FIFO has one more word for STMASK operand */
  unsigned_4* mask;
  
  mask = pke_pc_operand(me, 1);
  if(mask != NULL)
    {
      /* "transferring" operand */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_XFER);

      /* set NUM */
      PKE_REG_MASK_SET(me, NUM, NUM, 1);

      /* fill the register */
      PKE_REG_MASK_SET(me, MASK, MASK, *mask);

      /* set NUM */
      PKE_REG_MASK_SET(me, NUM, NUM, 0);

      /* done */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 1);
    }
  else
    {
      /* need to wait for another word */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* try again next cycle */
    }
}


void
pke_code_strow(struct pke_device* me, unsigned_4 pkecode)
{
  /* check that FIFO has four more words for STROW operand */
  unsigned_4* last_op;
  
  last_op = pke_pc_operand(me, 4);
  if(last_op != NULL)
    {
      /* "transferring" operand */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_XFER);
      
      /* set NUM */
      PKE_REG_MASK_SET(me, NUM, NUM, 1);

      /* copy ROW registers: must all exist if 4th operand exists */
      me->regs[PKE_REG_R0][0] = * pke_pc_operand(me, 1);
      me->regs[PKE_REG_R1][0] = * pke_pc_operand(me, 2);
      me->regs[PKE_REG_R2][0] = * pke_pc_operand(me, 3);
      me->regs[PKE_REG_R3][0] = * pke_pc_operand(me, 4);
      
      /* set NUM */
      PKE_REG_MASK_SET(me, NUM, NUM, 0);

      /* done */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 5);
    }
  else
    {
      /* need to wait for another word */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* try again next cycle */
    }
}


void
pke_code_stcol(struct pke_device* me, unsigned_4 pkecode)
{
  /* check that FIFO has four more words for STCOL operand */
  unsigned_4* last_op;
  
  last_op = pke_pc_operand(me, 4);
  if(last_op != NULL)
    {
      /* "transferring" operand */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_XFER);
      
      /* set NUM */
      PKE_REG_MASK_SET(me, NUM, NUM, 1);

      /* copy COL registers: must all exist if 4th operand exists */
      me->regs[PKE_REG_C0][0] = * pke_pc_operand(me, 1);
      me->regs[PKE_REG_C1][0] = * pke_pc_operand(me, 2);
      me->regs[PKE_REG_C2][0] = * pke_pc_operand(me, 3);
      me->regs[PKE_REG_C3][0] = * pke_pc_operand(me, 4);
      
      /* set NUM */
      PKE_REG_MASK_SET(me, NUM, NUM, 0);

      /* done */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 5);
    }
  else
    {
      /* need to wait for another word */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* try again next cycle */
    }
}


void
pke_code_mpg(struct pke_device* me, unsigned_4 pkecode)
{
  unsigned_4* last_mpg_word;
  int num = BIT_MASK_GET(pkecode, PKE_OPCODE_NUM_B, PKE_OPCODE_NUM_E);
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);

  /* map zero to max+1 */
  if(num==0) num=0x100;
  
  /* check that FIFO has a few more words for MPG operand */
  last_mpg_word = pke_pc_operand(me, num*2); /* num: number of 64-bit words */
  if(last_mpg_word != NULL)
    {
      /* perform implied FLUSHE */
      if(pke_check_stall(me, chk_vu))
	{
	  /* VU idle */
	  int i;
	  
	  /* "transferring" operand */
	  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_XFER);
	  
	  /* transfer VU instructions, one word per iteration */
	  for(i=0; i<num*2; i++)
	    {
	      address_word vu_addr_base, vu_addr;
	      address_word vutrack_addr_base, vutrack_addr;
	      unsigned_4* operand;
	      struct fifo_quadword* fq = pke_pc_fifo(me, num, & operand);
	      
	      /* set NUM */
	      PKE_REG_MASK_SET(me, NUM, NUM, (num*2 - i) / 2);
	      
	      /* imm: in 64-bit units for MPG instruction */
	      /* VU*_MEM0 : instruction memory */
	      vu_addr_base = (me->pke_number == 0) ?
		VU0_MEM0_WINDOW_START : VU0_MEM0_WINDOW_START;
	      vu_addr = vu_addr_base + (imm*2) + i;
	      
	      /* VU*_MEM0_TRACK : source-addr tracking table */
	      vutrack_addr_base = (me->pke_number == 0) ?
		VU0_MEM0_SRCADDR_START : VU1_MEM0_SRCADDR_START;
	      vutrack_addr = vu_addr_base + (imm*2) + i;
	      
	      /* write data into VU memory */
	      pke_track_write(me, operand, sizeof(unsigned_4),
			      vu_addr, fq->source_address);
	      
	      /* write srcaddr into VU srcaddr tracking table */
	      sim_write(NULL,
			(SIM_ADDR) vutrack_addr,
			(void*) & fq->source_address,
			sizeof(unsigned_4));
	    } /* VU xfer loop */

	  /* check NUM */
	  ASSERT(PKE_REG_MASK_GET(me, NUM, NUM) == 0);
	  
	  /* done */
	  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
	  pke_pc_advance(me, 1 + num*2);
	}
      else
	{
	  /* VU busy */
	  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_STALL);
	  /* retry this instruction next clock */
	}
    } /* if FIFO full enough */
  else
    {
      /* need to wait for another word */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* retry this instruction next clock */
    }
}


void
pke_code_direct(struct pke_device* me, unsigned_4 pkecode)
{
  /* check that FIFO has a few more words for DIRECT operand */
  unsigned_4* last_direct_word;
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
  int num = BIT_MASK_GET(pkecode, PKE_OPCODE_NUM_B, PKE_OPCODE_NUM_E);
  
  /* map zero to max+1 */
  if(imm==0) imm=0x10000;
  
  last_direct_word = pke_pc_operand(me, imm*4); /* num: number of 128-bit words */
  if(last_direct_word != NULL)
    {
      /* VU idle */
      int i;
      quadword fifo_data;
      
      /* "transferring" operand */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_XFER);
      
      /* transfer GPUIF quadwords, one word per iteration */
      for(i=0; i<imm*4; i++)
	{
	  unsigned_4* operand;
	  struct fifo_quadword* fq = pke_pc_fifo(me, num, &operand);
	  
	  /* collect word into quadword */
	  fifo_data[i%4] = *operand;
	  
	  /* write to GPUIF FIFO only with full word */
	  if(i%4 == 3)
	    {
	      address_word gpuif_fifo = GIF_PATH2_FIFO_ADDR+(i/4);
	      pke_track_write(me, fifo_data, sizeof(quadword),
			      (SIM_ADDR) gpuif_fifo, fq->source_address);
	    } /* write collected quadword */
	  
	} /* GPUIF xfer loop */
      
      /* done */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, 1 + imm*4);
    } /* if FIFO full enough */
  else
    {
      /* need to wait for another word */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* retry this instruction next clock */
    }
}


void
pke_code_directhl(struct pke_device* me, unsigned_4 pkecode)
{
  /* treat the same as DIRECTH */
  pke_code_direct(me, pkecode);
}


void
pke_code_unpack(struct pke_device* me, unsigned_4 pkecode)
{
  int imm = BIT_MASK_GET(pkecode, PKE_OPCODE_IMM_B, PKE_OPCODE_IMM_E);
  int cmd = BIT_MASK_GET(pkecode, PKE_OPCODE_CMD_B, PKE_OPCODE_CMD_E);
  int num = BIT_MASK_GET(pkecode, PKE_OPCODE_NUM_B, PKE_OPCODE_NUM_E);
  short vn = BIT_MASK_GET(cmd, 2, 3); /* unpack shape controls */
  short vl = BIT_MASK_GET(cmd, 0, 1);
  int m = BIT_MASK_GET(cmd, 4, 4);
  short cl = PKE_REG_MASK_GET(me, CYCLE, CL); /* cycle controls */
  short wl = PKE_REG_MASK_GET(me, CYCLE, WL);
  int r = BIT_MASK_GET(imm, 15, 15); /* indicator bits in imm value */
  int sx = BIT_MASK_GET(imm, 14, 14);

  int n, num_operands;
  unsigned_4* last_operand_word;
  
  /* map zero to max+1 */
  if(num==0) num=0x100;
  
  /* compute PKEcode length, as given in CPU2 spec, v2.1 pg. 11 */
  if(wl <= cl)
    n = num;
  else
    n = cl * (num/wl) + PKE_LIMIT(num % wl, cl);
  num_operands = (((sizeof(unsigned_4) >> vl) * (vn+1) * n)/sizeof(unsigned_4));
  
  /* confirm that FIFO has enough words in it */
  last_operand_word = pke_pc_operand(me, num_operands);
  if(last_operand_word != NULL)
    {
      address_word vu_addr_base;
      int vector_num;
      
      /* "transferring" operand */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_XFER);
      
      /* don't check whether VU is idle */

      /* compute VU address base */
      if(me->pke_number == 0)
	vu_addr_base = VU0_MEM1_WINDOW_START + BIT_MASK_GET(imm, 0, 9);
      else
	{
	  vu_addr_base = VU1_MEM1_WINDOW_START + BIT_MASK_GET(imm, 0, 9);
	  if(r) vu_addr_base += PKE_REG_MASK_GET(me, TOPS, TOPS);
	}

      /* set NUM */
      PKE_REG_MASK_SET(me, NUM, NUM, num);

      /* transfer given number of vectors */
      vector_num = 0;  /* output vector number being processed */
      do
	{
	  quadword vu_old_data;
	  quadword vu_new_data;
	  quadword unpacked_data;
	  address_word vu_addr;
	  unsigned_4 source_addr = 0;
	  int i;
	  
	  /* decrement NUM */
	  PKE_REG_MASK_SET(me, NUM, NUM,
			   PKE_REG_MASK_GET(me, NUM, NUM) - 1);
	      
	  /* compute VU destination address, as bytes in R5900 memory */
	  if(cl >= wl)
	    {
	      /* map zero to max+1 */
	      if(wl == 0) wl = 0x0100;
	      vu_addr = vu_addr_base + 16*(cl*(vector_num/wl) + (vector_num%wl));
	    }
	  else
	    vu_addr = vu_addr_base + 16*vector_num;

	  /* XXX: can vu_addr overflow? */
	  
	  /* read old VU data word at address */
	  sim_read(NULL, (SIM_ADDR) vu_addr, (void*) & vu_old_data, sizeof(vu_old_data));
	  
	  /* For cyclic unpack, next operand quadword may come from instruction stream
	     or be zero. */
	  if((cl < wl) && ((vector_num % wl) >= cl)) /* wl != 0, set above */
	    {
	      /* clear operand - used only in a "indeterminate" state */
	      for(i = 0; i < 4; i++)
		unpacked_data[i] = 0;
	    }
	  else
	    {
	      /* compute packed vector dimensions */
	      int vectorbits, unitbits;

	      if(vl < 3) /* PKE_UNPACK_*_{32,16,8} */
		{
		  unitbits = (32 >> vl);
		  vectorbits = unitbits * (vn+1);
		}
	      else if(vl == 3 && vn == 3) /* PKE_UNPACK_V4_5 */
		{
		  unitbits = 5;
		  vectorbits = 16;
		}
	      else /* illegal unpack variant */
		{
		  /* treat as illegal instruction */
		  pke_code_error(me, pkecode);
		  return;
		}
	      
	      /* loop over columns */
	      for(i=0; i<=vn; i++)
		{
		  unsigned_4 operand;

		  /* offset in bits in current operand word */
		  int bitoffset =
		    (vector_num * vectorbits) + (i * unitbits); /* # of bits from PKEcode */

		  /* last unit of V4_5 is only one bit wide */
		  if(vl == 3 && vn == 3 && i == 3) /* PKE_UNPACK_V4_5 */
		    unitbits = 1;

		  /* fetch bitfield operand */
		  operand = pke_pc_operand_bits(me, bitoffset, unitbits, & source_addr);

		  /* selectively sign-extend; not for V4_5 1-bit value */
		  if(sx && unitbits > 0)
		    unpacked_data[i] = SEXT32(operand, unitbits-1);
		  else
		    unpacked_data[i] = operand;
		}
	    } /* unpack word from instruction operand */
	  
	  /* compute replacement word */
	  if(m) /* use mask register? */
	    {
	      /* compute index into mask register for this word */
	      int mask_index = PKE_LIMIT(vector_num % wl, 3);  /* wl != 0, set above */
	      
	      for(i=0; i<3; i++) /* loop over columns */
		{
		  int mask_op = PKE_MASKREG_GET(me, mask_index, i);
		  unsigned_4* masked_value = NULL;
		  unsigned_4 zero = 0;
		  
		  switch(mask_op)
		    {
		    case PKE_MASKREG_INPUT: 
		      /* for vn == 0, all columns are copied from column 0 */
		      if(vn == 0)
			masked_value = & unpacked_data[0];
		      else if(i > vn)
			masked_value = & zero; /* arbitrary data: undefined in spec */
		      else
			masked_value = & unpacked_data[i];
		      break;
		      
		    case PKE_MASKREG_ROW: /* exploit R0..R3 contiguity */
		      masked_value = & me->regs[PKE_REG_R0 + i][0];
		      break;
		      
		    case PKE_MASKREG_COLUMN: /* exploit C0..C3 contiguity */
		      masked_value = & me->regs[PKE_REG_C0 + PKE_LIMIT(vector_num,3)][0];
		      break;
		      
		    case PKE_MASKREG_NOTHING:
		      /* "write inhibit" by re-copying old data */
		      masked_value = & vu_old_data[i];
		      break;
		      
		    default:
		      ASSERT(0);
		      /* no other cases possible */
		    }
		  
		  /* copy masked value for column */
		  vu_new_data[i] = *masked_value;
		} /* loop over columns */
	    } /* mask */
	  else
	    {
	      /* no mask - just copy over entire unpacked quadword */
	      memcpy(vu_new_data, unpacked_data, sizeof(unpacked_data));
	    }
	  
	  /* process STMOD register for accumulation operations */
	  switch(PKE_REG_MASK_GET(me, MODE, MDE))
	    {
	    case PKE_MODE_ADDROW: /* add row registers to output data */
	      for(i=0; i<4; i++)
		/* exploit R0..R3 contiguity */
		vu_new_data[i] += me->regs[PKE_REG_R0 + i][0];
	      break;

	    case PKE_MODE_ACCROW: /* add row registers to output data; accumulate */
	      for(i=0; i<4; i++)
		{
		  /* exploit R0..R3 contiguity */
		  vu_new_data[i] += me->regs[PKE_REG_R0 + i][0];
		  me->regs[PKE_REG_R0 + i][0] = vu_new_data[i];
		}
	      break;

	    case PKE_MODE_INPUT: /* pass data through */
	    default:
	      ;
	    }

	  /* write replacement word */
	  pke_track_write(me, vu_new_data, sizeof(vu_new_data),
			  (SIM_ADDR) vu_addr, source_addr);

	  /* next vector please */
	  vector_num ++;
	} /* vector transfer loop */
      while(PKE_REG_MASK_GET(me, NUM, NUM) > 0);

      /* done */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
      pke_pc_advance(me, num_operands);
    } /* PKE FIFO full enough */
  else
    {
      /* need to wait for another word */
      PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_WAIT);
      /* retry this instruction next clock */
    }
}


void
pke_code_error(struct pke_device* me, unsigned_4 pkecode)
{
  /* set ER1 flag in STAT register */
  PKE_REG_MASK_SET(me, STAT, ER1, 1);
  /* advance over faulty word */
  PKE_REG_MASK_SET(me, STAT, PPS, PKE_REG_STAT_PPS_IDLE);
  pke_pc_advance(me, 1);
}
