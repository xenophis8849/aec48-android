// Aec48Uploader 的主机测试（不需要 Android）：本机起一个临时 HTTP 服务数收到的上传请求。
// 运行：javac -d /tmp/up android/src/com/karaoke/aec48/Aec48Uploader.java test/Aec48UploaderTest.java && java -cp /tmp/up Aec48UploaderTest
import com.karaoke.aec48.Aec48Uploader;
import com.sun.net.httpserver.HttpServer;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicInteger;

public class Aec48UploaderTest {
    static int fails = 0;
    static void check(boolean ok, String what, String detail) { System.out.println((ok ? "通过    " : "不通过  ") + what + (detail.isEmpty() ? "" : "：" + detail)); if (!ok) fails++; }
    static void put(File f, byte[] b) throws Exception { try (FileOutputStream o = new FileOutputStream(f)) { o.write(b); } }

    public static void main(String[] a) throws Exception {
        AtomicInteger posts = new AtomicInteger(); AtomicInteger status = new AtomicInteger(200);
        HttpServer srv = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        srv.createContext("/up", ex -> { try (InputStream in = ex.getRequestBody()) { in.readAllBytes(); } posts.incrementAndGet(); ex.sendResponseHeaders(status.get(), -1); ex.close(); });
        srv.start();
        String url = "http://127.0.0.1:" + srv.getAddress().getPort() + "/up";
        Aec48Uploader up = new Aec48Uploader(url, "testdb", "host", "test");
        File dir = Files.createTempDirectory("aec48up").toFile();
        String[] names = {"mic.wav", "ref.wav", "out.wav", "meta.json"};
        for (String n : names) put(new File(dir, n), ("content-" + n).getBytes("UTF-8"));
        try {
            posts.set(0); int r = up.upload(dir, "s1", null);
            check(r == 0 && posts.get() == 4, "首次上传：4 个文件全传、返回 0", "返回 " + r + "，请求 " + posts.get());
            posts.set(0); r = up.upload(dir, "s1", null);
            check(r == 0 && posts.get() == 0, "同一会话重试：已传的不重传", "返回 " + r + "，请求 " + posts.get());
            posts.set(0); r = up.upload(dir, "s2", null);
            check(r == 0 && posts.get() == 4, "换一个会话（旧标记还在）：4 个文件重新上传", "返回 " + r + "，请求 " + posts.get());
            File mic = new File(dir, "mic.wav"); long mt = mic.lastModified(); byte[] b = Files.readAllBytes(mic.toPath()); b[0] ^= 1; put(mic, b); mic.setLastModified(mt);
            posts.set(0); r = up.upload(dir, "s2", null);
            check(r == 0 && posts.get() == 1, "同会话、内容变了但大小和修改时间不变：只重传这一个", "返回 " + r + "，请求 " + posts.get());
            posts.set(0); r = up.upload(dir, null, null);
            check(r == 0 && posts.get() == 0, "不传 sessionId：沿用目录里记的会话，不重传", "返回 " + r + "，请求 " + posts.get());
            status.set(500); posts.set(0); r = up.upload(dir, "s3", null);
            check(r == 4 && posts.get() == 4, "接收端报错：4 个都计失败", "返回 " + r + "，请求 " + posts.get());
            status.set(200); posts.set(0); r = up.upload(dir, "s3", null);
            check(r == 0 && posts.get() == 4, "接收端恢复后同会话补传：4 个都传", "返回 " + r + "，请求 " + posts.get());
            new File(dir, "meta.json").delete(); posts.set(0); r = up.upload(dir, "s4", null);
            check(r == 1 && posts.get() == 3, "缺 meta.json：计 1 个失败，其余照传", "返回 " + r + "，请求 " + posts.get());
            put(new File(dir, "meta.json"), "content-meta".getBytes("UTF-8")); put(new File(dir, "out_prehook.wav"), "content-pre".getBytes("UTF-8"));
            posts.set(0); r = up.upload(dir, "s5", null);
            check(r == 0 && posts.get() == 5, "有 out_prehook.wav（挂着频谱回调录的）：5 个都传", "返回 " + r + "，请求 " + posts.get());
        } finally {
            srv.stop(0); for (File f : dir.listFiles()) f.delete(); dir.delete();
        }
        System.out.println((fails == 0 ? "全部通过" : "有问题") + "：不通过 " + fails + " 项"); System.exit(fails == 0 ? 0 : 1);
    }
}
