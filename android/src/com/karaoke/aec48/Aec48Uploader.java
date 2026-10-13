package com.karaoke.aec48;

import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.zip.CRC32;

/**
 * 录音上传（aec48_dump_stop 之后调用）：把录音目录里的 mic.wav / ref.wav / out.wav / meta.json 逐个上传；
 * 有 out_prehook.wav（录音时挂着频谱回调，0.5 起）也一并上传，没有不算失败。
 *
 * 两项配置由接入方填：
 *   uploadUrl —— 接收地址（HTTP/HTTPS），例如 https://内部域名/aec/upload
 *   dbName    —— 库名，原样放进每个请求的 db 字段，接收端据此决定存到哪个库 / 桶
 *
 * 请求格式（接收端按这个收）：每个文件一个 POST，multipart/form-data，字段：
 *   db           库名（上面的 dbName）
 *   session_id   一次录音的编号（同一次录音的 4 个文件相同）
 *   track        mic | ref | out | meta
 *   device       手机型号（Build.MANUFACTURER + " " + Build.MODEL）
 *   app_version  接入方 App 版本号
 *   file         文件本体（wav 或 json）
 * 成功 = HTTP 2xx。失败的文件保留在目录里，可以下次再调 upload 补传。已成功的文件写 <文件名>.uploaded 标记（内容 = session_id、文件大小与内容 CRC32），
 * 只有三者都与这次一致才跳过：同一会话重试只补传失败的文件；换一个 session_id 或文件内容变了（同一目录重新录音）都会重新上传。
 * 四个文件缺任何一个都计为失败（录音不完整不会报成功）。
 * 建议每次录音用一个新目录。
 * 不能在主线程调用（会做网络请求）。
 */
public final class Aec48Uploader {
    public interface Progress { void onFile(String track, boolean ok, String detail); }

    private final String uploadUrl, dbName, device, appVersion;

    public Aec48Uploader(String uploadUrl, String dbName, String device, String appVersion) {
        this.uploadUrl = uploadUrl; this.dbName = dbName; this.device = device; this.appVersion = appVersion;
    }

    /** 上传一个录音目录。返回失败的文件数（0 = 全部成功）。sessionId 传 null 时自动生成并写进目录（重传时沿用）。 */
    public int upload(File dir, String sessionId, Progress cb) {
        File sidFile = new File(dir, ".session_id");
        try {
            if (sessionId == null) sessionId = sidFile.exists() ? readText(sidFile).trim() : UUID.randomUUID().toString();
            writeText(sidFile, sessionId);
        } catch (Exception e) { if (cb != null) cb.onFile("-", false, "写 session_id 失败：" + e); return 4; }
        String[][] files = {{"mic", "mic.wav"}, {"ref", "ref.wav"}, {"out", "out.wav"}, {"meta", "meta.json"}, {"out_prehook", "out_prehook.wav", "optional"}};
        int failed = 0;
        for (String[] t : files) {
            File f = new File(dir, t[1]), mark = new File(dir, t[1] + ".uploaded");
            if (!f.isFile()) { if (t.length > 2) continue; failed++; if (cb != null) cb.onFile(t[0], false, "缺文件 " + t[1]); continue; }
            String stamp;
            try { stamp = sessionId + "|" + f.length() + "|" + crc32(f); }
            catch (Exception e) { failed++; if (cb != null) cb.onFile(t[0], false, "读文件失败：" + e); continue; }
            try { if (mark.exists() && readText(mark).trim().equals(stamp)) { if (cb != null) cb.onFile(t[0], true, "已传过"); continue; } } catch (Exception ignored) { }
            try {
                int code = post(f, t[0], sessionId);
                boolean ok = code >= 200 && code < 300;
                if (ok) writeText(mark, stamp); else failed++;
                if (cb != null) cb.onFile(t[0], ok, "HTTP " + code);
            } catch (Exception e) { failed++; if (cb != null) cb.onFile(t[0], false, e.toString()); }
        }
        return failed;
    }

    private int post(File f, String track, String sessionId) throws Exception {
        String b = "----aec48" + UUID.randomUUID().toString().replace("-", "");
        HttpURLConnection c = (HttpURLConnection) new URL(uploadUrl).openConnection();
        c.setRequestMethod("POST"); c.setDoOutput(true); c.setConnectTimeout(15000); c.setReadTimeout(120000);
        c.setChunkedStreamingMode(1 << 16); c.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + b);
        try (OutputStream os = c.getOutputStream(); DataOutputStream o = new DataOutputStream(os)) {
            List<String[]> fields = new ArrayList<>();
            fields.add(new String[]{"db", dbName}); fields.add(new String[]{"session_id", sessionId}); fields.add(new String[]{"track", track});
            fields.add(new String[]{"device", device}); fields.add(new String[]{"app_version", appVersion});
            for (String[] kv : fields) {
                o.write(("--" + b + "\r\nContent-Disposition: form-data; name=\"" + kv[0] + "\"\r\n\r\n" + kv[1] + "\r\n").getBytes("UTF-8"));
            }
            String ct = f.getName().endsWith(".json") ? "application/json" : "audio/wav";
            o.write(("--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + f.getName() + "\"\r\nContent-Type: " + ct + "\r\n\r\n").getBytes("UTF-8"));
            try (InputStream in = new FileInputStream(f)) { byte[] buf = new byte[1 << 16]; int n; while ((n = in.read(buf)) > 0) o.write(buf, 0, n); }
            o.write(("\r\n--" + b + "--\r\n").getBytes("UTF-8"));
        }
        int code = c.getResponseCode(); c.disconnect(); return code;
    }

    private static String crc32(File f) throws Exception {
        CRC32 c = new CRC32(); try (InputStream in = new FileInputStream(f)) { byte[] b = new byte[1 << 16]; int n; while ((n = in.read(b)) > 0) c.update(b, 0, n); }
        return Long.toHexString(c.getValue());
    }
    private static String readText(File f) throws Exception { try (InputStream in = new FileInputStream(f)) { byte[] b = new byte[(int) f.length()]; int n = in.read(b); return new String(b, 0, Math.max(n, 0), "UTF-8"); } }
    private static void writeText(File f, String s) throws Exception { try (java.io.FileOutputStream o = new java.io.FileOutputStream(f)) { o.write(s.getBytes("UTF-8")); } }
}
