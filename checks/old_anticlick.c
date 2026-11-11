#include "../test-dev/test_mixer_note_cut_ac.c"
int main(void) {
 int result=_test_func_test_mixer_note_cut_ac();
 if(result==0) puts("OLD_NOTE_CUT_ANTICLICK_OK");
 return result;
}
