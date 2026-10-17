/* SPDX-License-Identifier: GPL-2.0-only */
static void test_dma(int which)
{
    if(!setjmp(stopped)) {
        if(which) dma_write_error(-5,0); else dma_read_error(-5,0);
        assert(!"DMA error returned without reset and could release/reuse storage");
    }
    assert(!mode_off);
}
static void test_wait(int which)
{
    u8 scratch=0, status=0;
    struct spinand_device s={.scratchbuf=&scratch};
    status_value=1; read_error=which ? 0 : -5;
    if(!setjmp(stopped)) {
        spinand_wait(&s,0,1,&status);
        assert(!"unknown NAND completion returned to a recovery caller");
    }
}
int main(void)
{
    test_dma(0); test_dma(1);
    assert(dma_read_error(-5,1)==-5 && dma_write_error(-5,1)==-5);
    assert(mode_off==2); /* errors before mapping remain ordinary failures */
    test_wait(0); test_wait(1);
    for(int value=0;value<=8;value+=4) {
        u8 scratch=0,status=255;
        struct spinand_device s={.scratchbuf=&scratch};
        status_value=value; read_error=0;
        assert(!spinand_wait(&s,0,1,&status) && status==value);
    }
    assert(panics==4);
    puts("DMA lease retained on error; unknown chip completion stops; completed media errors preserved");
    return 0;
}
