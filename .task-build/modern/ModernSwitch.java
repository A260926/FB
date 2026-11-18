/*   1:   0 */ public class ModernSwitch {
/*   2:   0 */   public static int choose(int paramInt) {
/*   3:   0 */     return 10 + switch (paramInt){
/*   4:   0 */         case 0, 1 -> 11;
/*   5:   0 */       case 100 -> {
/*   6:   0 */         int i = paramInt + 1;
/*   7:   0 */         yield i;
/*   8:   0 */       }
/*   9:   0 */     default -> 17;
/*  10:   0 */ };
/*  11:   0 */ }
/*  12:   0 */ }
