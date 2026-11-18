/*   1:   0 */ public class LegacySwitch {
/*   2:   0 */   public static void mark(int paramInt) {}
/*   3:   0 */   
/*   4:   0 */   public static void choose(int paramInt) {
/*   5:   0 */     switch (paramInt) {
/*   6:   0 */       case 1:
/*   7:   0 */         mark(2);
/*   8:   0 */       case 2:
/*   9:   0 */         mark(3);
/*  10:   0 */         break;
/*  11:   0 */       default:
/*  12:   0 */         mark(7);
/*  13:   0 */         break;
/*  14:   0 */     } 
/*  15:   0 */   }
/*  16:   0 */ }
