#include "../tests/ltr_cases.c"

int main(int argc, char **argv)
{
  FILE *f = fopen(argv[1], "rb");
  ByteBuffer b = {0};
  unsigned char tmp[4096];
  size_t n;
  while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf_append(&b, tmp, n);
  fclose(f);

  ParsedNal *nals;
  int count;
  extract_nals(&b, &nals, &count);
  for (int i = 0; i < count; ++i) {
    if (nals[i].type == 33) {
      SpsInfo s = parse_sps_nal(&nals[i]);
      printf("SPS bits=%d lt=%d spslt=%d maxdpbminus1=%d\n", s.poc_bits, s.lt_present, s.sps_lt_count, s.max_dpb_minus1);
    }
  }
  SpsInfo sps = {0};
  for (int i = 0; i < count; ++i) if (nals[i].type ==33) sps=parse_sps_nal(&nals[i]);
  int poc = 0;
  for (int i = 0; i < count; ++i) {
    if (nals[i].type == 1 || nals[i].type == 19 || nals[i].type ==20) {
      SliceInfo s = parse_slice_nal(&nals[i], &sps);
      if (poc % 8 == 0 || poc == 32 || poc == 16) {
        printf("slice coding=%d type=%d lsb=%d nlt=%d", poc, s.nal_type, s.poc_lsb, s.num_lt);
        for (int j=0;j<s.num_lt;j++) printf(" [lsb=%d used=%d pres=%d delta=%d full=%d]",
          s.lt_lsb[j],s.lt_used[j],s.lt_msb_present[j],s.lt_delta[j],
          lt_full_poc(&sps,&s,j,poc));
        printf("\n");
      }
      poc++;
    }
  }
  return 0;
}
