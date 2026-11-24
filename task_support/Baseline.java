import task_support.Inputs;
import cz.muni.stanse.codestructures.*;
import cz.muni.stanse.checker.CheckerFactory;
import java.util.*;
public final class Baseline {
 public static void main(String[] args) throws Exception {
  List<CFGHandle> cfgs=Inputs.parse("baseline_safe","void safe(void){int a[4];int i;for(i=0;i<4;++i)a[i]=0;}");
  if(cfgs.size()!=1)throw new AssertionError("one function required");
  int branches=0,edges=0,accesses=0;
  for(CFGHandle c:cfgs)for(CFGNode n:c.getAllNodes()){
   if(n instanceof CFGBranchNode)branches++;
   edges+=n.getSuccessors().size();accesses+=n.getElement().selectNodes(".//arrayAccess").size();
  }
  if(branches<1||edges<5||accesses!=1)throw new AssertionError("real production CFG required");
  System.out.println("CFG functions="+cfgs.size()+" branches="+branches+" edges="+edges+" accesses="+accesses);
  System.out.println("CHECKERS="+new TreeSet<String>(CheckerFactory.getRegisteredCheckers()));
  System.out.println("STANSE_BASELINE_OK");
 }
}
