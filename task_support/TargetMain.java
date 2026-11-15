public final class TargetMain {
    public static void main(String[] args) throws Exception {
        TaskChecks.group("deformation", SurfaceAttachmentTests::deformation);
        TaskChecks.group("frames", SurfaceAttachmentTests::frames);
        TaskChecks.group("lifecycle", SurfaceAttachmentTests::lifecycle);
        System.out.println("TARGET_COMPLETE groups=3");
    }
}
