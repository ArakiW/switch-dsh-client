#!/usr/bin/env node
/**
 * dsh-bridge — 把 DeepSeek Harness 的回环 API(默认 http://127.0.0.1:3080)
 * 转发到局域网,并把下行 WebSocket 事件流翻译成 SSE,供 Switch 客户端(libcurl)消费。
 *
 * 为什么需要它:
 *   - DSH 的 Web CLI 显式拒绝 --host 0.0.0.0,HTTP/WS 服务只绑 127.0.0.1;
 *   - 下行事件流只有 WebSocket(/api/events.mux,下行单向),而 Switch 侧
 *     用 libcurl 收 SSE 比手写 RFC6455 客户端简单可靠得多。
 *
 * 端点:
 *   POST /api/<method>                    -> JSON 信封原样转发到 DSH
 *   GET  /api/events.mux                  -> WebSocket 原始双向转发(调试用)
 *   GET  /api/events.sse[?sessionId=x]    -> WS 帧 -> SSE(data: <json>),可选按会话过滤
 *   GET  /                                -> 状态页
 *
 * 用法:
 *   node dsh-bridge.js [--dsh http://127.0.0.1:3080] [--host 0.0.0.0] [--port 8765]
 *   环境变量:DSH_URL / BRIDGE_HOST / BRIDGE_PORT(命令行参数优先)
 *
 * 零依赖(node:http/node:crypto)。本进程不内置鉴权,与 DSH 的信任模型一致,
 * 只应在可信局域网内运行;不要暴露到公网(DSH API 可执行远程代码)。
 */
'use strict';

const http = require('http');
const crypto = require('crypto');
const { URL } = require('url');

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

/* ---------- 参数 ---------- */

function argVal(name) {
  const i = process.argv.indexOf(name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : undefined;
}

const dshUrl = new URL(argVal('--dsh') || process.env.DSH_URL || 'http://127.0.0.1:3080');
const listenHost = argVal('--host') || process.env.BRIDGE_HOST || '0.0.0.0';
const listenPort = parseInt(argVal('--port') || process.env.BRIDGE_PORT || '8765', 10);

if (dshUrl.protocol !== 'http:') {
  console.error(`dsh-bridge: 仅支持 http:// 上游(DSH 本身无 TLS),收到: ${dshUrl.protocol}`);
  process.exit(1);
}

const upstream = {
  hostname: dshUrl.hostname,
  port: dshUrl.port ? parseInt(dshUrl.port, 10) : 80,
  pathPrefix: dshUrl.pathname.replace(/\/$/, ''),
};

/* ---------- 上游 WebSocket 连接 ---------- */

function connectUpstreamWs(path) {
  const key = crypto.randomBytes(16).toString('base64');
  return new Promise((resolve, reject) => {
    const req = http.request({
      hostname: upstream.hostname,
      port: upstream.port,
      path: upstream.pathPrefix + path,
      headers: {
        'connection': 'Upgrade',
        'upgrade': 'websocket',
        'sec-websocket-key': key,
        'sec-websocket-version': '13',
      },
    });
    req.on('upgrade', (_res, socket, head) => resolve({ socket, head }));
    req.on('error', reject);
    req.end();
  });
}

function wsAccept(key) {
  return crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
}

/* ---------- WS 帧工具 ---------- */

/* 客户端->服务端方向的帧(RFC6455:必须掩码) */
function writeFrame(socket, opcode, payload) {
  const len = payload.length;
  const mask = crypto.randomBytes(4);
  const masked = Buffer.alloc(len);
  for (let i = 0; i < len; i++) masked[i] = payload[i] ^ mask[i & 3];

  let header;
  if (len < 126) {
    header = Buffer.from([0x80 | opcode, 0x80 | len]);
  } else if (len < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 126;
    header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  socket.write(Buffer.concat([header, mask, masked]));
}

/*
 * 增量帧解析器,onFrame(frame):
 *   { op:'data', payload, fin }  文本/二进制帧(payload 为 Buffer)
 *   { op:'ping', payload } / { op:'close', payload } / pong 忽略
 * 处理 16/64 位长度;服务端帧不掩码(掩码帧按规范丢弃)。
 */
function makeFrameParser(onFrame) {
  let buf = Buffer.alloc(0);
  let fragments = [];

  return (chunk) => {
    buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
    for (;;) {
      if (buf.length < 2) return;
      const b0 = buf[0];
      const b1 = buf[1];
      const fin = (b0 & 0x80) !== 0;
      const opcode = b0 & 0x0f;
      const masked = (b1 & 0x80) !== 0;
      let len = b1 & 0x7f;
      let off = 2;

      if (len === 126) {
        if (buf.length < off + 2) return;
        len = buf.readUInt16BE(off);
        off += 2;
      } else if (len === 127) {
        if (buf.length < off + 8) return;
        const big = buf.readBigUInt64BE(off);
        off += 8;
        if (big > BigInt(Number.MAX_SAFE_INTEGER)) { buf = Buffer.alloc(0); return; }
        len = Number(big);
      }
      if (masked) {
        if (buf.length < off + 4) return;
        buf = Buffer.alloc(0); /* 服务端帧不应掩码;丢弃并重置 */
        return;
      }
      if (buf.length < off + len) return;

      const payload = buf.subarray(off, off + len);
      buf = buf.subarray(off + len);

      if (opcode === 8) { onFrame({ op: 'close', payload }); return; }
      if (opcode === 9) { onFrame({ op: 'ping', payload }); continue; }
      if (opcode === 10) continue; /* pong */

      if (opcode === 0) {
        /* 续帧 */
        fragments.push(payload);
        if (fin) {
          onFrame({ op: 'data', payload: Buffer.concat(fragments), fin: true });
          fragments = [];
        }
        continue;
      }
      if (opcode === 1 || opcode === 2) {
        if (!fin) { fragments.push(payload); continue; }
        onFrame({ op: 'data', payload, fin: true });
        continue;
      }
      /* 其它控制帧忽略 */
    }
  };
}

/* ---------- 请求处理 ---------- */

function relayPost(req, res) {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('error', () => res.destroy());
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    const up = http.request({
      hostname: upstream.hostname,
      port: upstream.port,
      path: upstream.pathPrefix + req.url,
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'content-length': body.length,
      },
    }, (ures) => {
      res.writeHead(ures.statusCode || 200, {
        'content-type': ures.headers['content-type'] || 'application/json',
        'cache-control': 'no-store',
      });
      ures.pipe(res);
    });
    up.on('error', (e) => {
      if (res.writableEnded) return;
      res.writeHead(502, { 'content-type': 'application/json' });
      res.end(JSON.stringify({
        type: 'server-response',
        rpcId: null,
        result: {
          ok: false,
          error: { code: 'bridge-upstream', message: `bridge 无法连接 DSH: ${e.message}` },
        },
      }));
    });
    up.end(body);
  });
}

function handleSse(req, res, sessionFilter) {
  res.writeHead(200, {
    'content-type': 'text/event-stream; charset=utf-8',
    'cache-control': 'no-cache',
    'connection': 'keep-alive',
    'x-accel-buffering': 'no',
  });
  res.write(': connected to dsh-bridge\n\n');

  const keepalive = setInterval(() => {
    if (!res.writableEnded) res.write(': keepalive\n\n');
  }, 20000);

  let closed = false;
  const finish = () => {
    if (closed) return;
    closed = true;
    clearInterval(keepalive);
    if (!res.writableEnded) res.end();
  };

  const parse = makeFrameParser((frame) => {
    if (frame.op === 'ping') { writeFrame(wsock, 0xa, frame.payload); return; } /* pong=0xA */
    if (frame.op === 'close') { finish(); return; }
    if (frame.op !== 'data') return;

    const text = frame.payload.toString('utf8');
    if (sessionFilter) {
      try {
        const msg = JSON.parse(text);
        if (msg && msg.payload && msg.payload.sessionId !== sessionFilter) return;
      } catch (_) { /* 非 JSON 帧原样放行 */ }
    }
    res.write(`data: ${text}\n\n`);
  });

  let wsock = null;
  connectUpstreamWs('/api/events.mux').then(({ socket, head }) => {
    wsock = socket;
    socket.on('data', parse);
    socket.on('error', (e) => {
      if (!res.writableEnded) {
        res.write(`event: error\ndata: ${JSON.stringify({ message: '上游 WS 错误: ' + e.message })}\n\n`);
      }
      finish();
    });
    socket.on('close', finish);
    if (head && head.length) socket.unshift(head);
  }).catch((e) => {
    if (!res.writableEnded) {
      res.write(`event: error\ndata: ${JSON.stringify({ message: 'bridge 无法连接 DSH WebSocket: ' + e.message })}\n\n`);
    }
    finish();
  });

  req.on('close', finish);
  res.on('close', finish);
}

function handleWsRelay(req, socket, head) {
  connectUpstreamWs('/api/events.mux').then(({ socket: usock, head: uhead }) => {
    const accept = wsAccept(req.headers['sec-websocket-key'] || '');
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`
    );
    if (uhead && uhead.length) usock.unshift(uhead);
    socket.on('error', () => usock.destroy());
    usock.on('error', () => socket.destroy());
    socket.on('close', () => usock.destroy());
    usock.on('close', () => socket.destroy());
    socket.pipe(usock);
    usock.pipe(socket);
  }).catch(() => {
    socket.write('HTTP/1.1 502 Bad Gateway\r\n\r\n');
    socket.destroy();
  });
}

function handleInfo(res) {
  res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
  res.end(
    '<!doctype html><meta charset="utf-8"><title>dsh-bridge</title>' +
    `<h2>dsh-bridge</h2><p>上游 DSH: <code>${dshUrl.href}</code></p>` +
    '<p>端点: <code>POST /api/&lt;method&gt;</code>、<code>GET /api/events.sse[?sessionId=x]</code>、<code>GET /api/events.mux</code></p>' +
    '<p>仅限可信局域网使用,勿暴露公网。</p>'
  );
}

/* ---------- 服务器 ---------- */

const server = http.createServer((req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  if (req.method === 'POST' && path.startsWith('/api/')) return relayPost(req, res);
  if (req.method === 'GET' && path === '/api/events.sse') {
    return handleSse(req, res, new URL(req.url, 'http://x').searchParams.get('sessionId'));
  }
  if (req.method === 'GET' && (path === '/' || path === '/index.html')) return handleInfo(res);
  res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
  res.end('not found\n');
});

server.on('upgrade', (req, socket, head) => {
  const path = new URL(req.url, 'http://x').pathname;
  if (path === '/api/events.mux') return handleWsRelay(req, socket, head);
  socket.write('HTTP/1.1 404 Not Found\r\n\r\n');
  socket.destroy();
});

server.on('error', (e) => {
  console.error('dsh-bridge server error:', e.message);
  process.exit(1);
});

server.listen(listenPort, listenHost, () => {
  console.log(`dsh-bridge 已启动: http://${listenHost}:${listenPort}  ->  ${dshUrl.href}`);
  console.log('Switch 端把 config.json 的 harness_base_url 配成: http://<本机局域网IP>:' + listenPort);
  console.log('仅限可信局域网,勿暴露公网。');
});
