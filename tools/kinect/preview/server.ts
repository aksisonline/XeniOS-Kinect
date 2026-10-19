// bun run server.ts → http://localhost:8765
import { spawn } from "bun";
const D = 640 * 480 * 2, F = D + 640 * 480 * 3;
const proc = spawn(["./kstream"], { stdout: "pipe", stderr: "inherit" });
const server = Bun.serve({
  port: 8765,
  fetch: (req, s) => s.upgrade(req) ? undefined : new Response(Bun.file("index.html")),
  websocket: { open: (w) => w.subscribe("f"), message() {} },
});
let buf = new Uint8Array(F * 2), n = 0;
for await (const c of proc.stdout) {
  buf.set(c, n); n += c.length;
  while (n >= F) {
    server.publish("f", buf.slice(0, F));
    buf.copyWithin(0, F, n); n -= F;
  }
}
console.error("kstream exited"); process.exit(1);
