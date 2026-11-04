static int bb_hold_reset(void)
{
 uint32_t io,got;
 if(!identify_panel.pins_owned)return 0;
 if(sm8750_panel_raw_read(0,0xa1004,&io)||
    sm8750_panel_raw_write(0,0xa1004,io&~2u)||
    sm8750_panel_raw_read(0,0xa1004,&got)||(got&2u))return -1;
 return 0;
}
static int bb_cleanup_all(void)
{
 /* Supplies stay owned until SPI pins are restored. */
 if(bb_hold_reset())return -1;
 if(op13_bitbang_cleanup(&bb_state))return -1;
 if(sm8750_panel_release(&identify_panel))return -1;
 identify_session.panel_owned=0;
 identify_session.blocked=0;
 return 0;
}
