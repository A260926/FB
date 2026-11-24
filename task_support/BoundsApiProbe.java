import task_support.Inputs;
import cz.muni.stanse.boundschecker.BoundsChecker;
import cz.muni.stanse.boundschecker.BoundsFinding;
import cz.muni.stanse.codestructures.CFGHandle;
import cz.muni.stanse.codestructures.CFGNode;
import cz.muni.stanse.checker.CheckerFactory;
import java.util.*;
import cz.muni.stanse.pointeranalyzer.*;
import cz.muni.stanse.pointeranalyzer.shapirohorwitz.*;
import cz.muni.stanse.utils.Pair;
public final class BoundsApiProbe {
 public static void main(String[] args) throws Exception {
  Object checker=CheckerFactory.createIntraprocedural("BoundsChecker",Collections.<java.io.File>emptyList());
  if(!(checker instanceof BoundsChecker))throw new AssertionError("factory not integrated");
  CFGHandle cfg=Inputs.parse("api_normal","void api_normal(void){int a[4];int *p=a;int i=2;p[i]=0;}").get(0);
  PointsToAnalyzer provider=new ShapiroHorwitzAnalyzer(new SteensgaardCategorizationProvider());
  provider.analyze(Collections.singletonList(cfg));
  CFGNode access=null;
  for(CFGNode node:cfg.getAllNodes())if(!node.getElement().selectNodes(".//arrayAccess").isEmpty())access=node;
  if(access==null)throw new AssertionError("real access node required");
  Set<Pair<CFGHandle,String>> origins=provider.getPointsToSetAt(cfg,access,"p");
  if(origins==null||!origins.contains(Pair.make(cfg,"a")))throw new AssertionError("program-point array origin required");
  List<BoundsFinding> findings=((BoundsChecker)checker).analyze(cfg);
  if(findings.size()!=1||!"proved".equals(findings.get(0).getStatus()))throw new AssertionError("normal access proof required");
  System.out.println("BOUNDS_API_NORMAL_OK");
 }
}
