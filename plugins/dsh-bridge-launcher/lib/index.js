/*
 * dsh-bridge-launcher(host 半身)。
 *
 * 无需客户端构建:通过 webServer 注册几个同源路由,并用 tapIndex
 * 向 DSH 网页注入一个原生 JS 浮动按钮(右下角,可拖拽),实现
 * dsh-bridge 的一键启动/停止/状态显示。
 *
 * 安装:dsh plugin --profile web add github:ArakiW/switch-dsh-client
 * 生效:重启 DSH。桥接脚本路径默认取本仓库 bridge/dsh-bridge.js,
 * 可用环境变量 DSH_BRIDGE_SCRIPT 覆盖。
 */
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..', '..', '..');
const bridgeScript =
  process.env.DSH_BRIDGE_SCRIPT || join(repoRoot, 'bridge', 'dsh-bridge.js');

function json(res, obj) {
  res.writeHead(200, { 'content-type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(obj));
}

/* 注入到网页的按钮脚本(原生 JS,无依赖) */
const BUTTON_JS = String.raw`
(function () {
  if (window.__dshBridgeButton) return;
  window.__dshBridgeButton = true;

  function el(tag, style, text) {
    var n = document.createElement(tag);
    if (style) for (var k in style) n.style[k] = style[k];
    if (text !== undefined) n.textContent = text;
    return n;
  }

  var pill = el('button', {
    position: 'fixed', right: '20px', bottom: '200px', zIndex: '9999',
    display: 'flex', alignItems: 'center', gap: '10px',
    border: '1px solid rgba(128,128,128,0.55)', borderRadius: '26px',
    padding: '12px 22px', background: 'rgba(28,30,42,0.94)', color: '#f0f2f8',
    fontSize: '16px', fontWeight: '600', cursor: 'pointer',
    boxShadow: '0 6px 20px rgba(0,0,0,0.4)', whiteSpace: 'nowrap',
    userSelect: 'none', touchAction: 'none'
  });
  pill.title = '点击启动/停止 dsh-bridge(局域网 8765,Switch 连 Harness 用);可按住拖动';
  var dot = el('span', { fontSize: '18px' }, '\u25B6');
  var label = el('span', null, 'Switch \u6865\u63A5:\u52A0\u8F7D\u4E2D...');
  pill.appendChild(dot);
  pill.appendChild(label);
  document.body.appendChild(pill);

  var running = false, busy = false, msg = '';
  function setUI() {
    dot.style.color = running ? '#d4ffdd' : '#b9c2d4';
    dot.textContent = running ? '\u25CF' : '\u25B6';
    label.textContent = busy ? '\u64CD\u4F5C\u4E2D...'
      : (running ? 'Switch \u6865\u63A5:\u8FD0\u884C\u4E2D(\u70B9\u51FB\u505C\u6B62)'
                 : 'Switch \u6865\u63A5:\u5DF2\u505C\u6B62(\u70B9\u51FB\u542F\u52A8)');
    pill.style.background = running ? 'rgba(46,160,67,0.94)' : 'rgba(28,30,42,0.94)';
    pill.title = msg || pill.title;
  }
  function refresh() {
    fetch('/bridge-launcher/status').then(function (r) { return r.json(); }).then(function (s) {
      running = !!s.running; msg = s.log || ''; setUI();
    }).catch(function () { msg = 'status \u8BF7\u6C42\u5931\u8D25'; setUI(); });
  }
  function act(path) {
    if (busy) return;
    busy = true; setUI();
    fetch(path, { method: 'POST' }).then(function (r) { return r.json(); }).then(function (s) {
      running = !!s.running;
      msg = (s.msg && ['started','stopped','not-running','already-running'].indexOf(s.msg) === -1) ? s.msg : '';
      busy = false; setUI();
    }).catch(function () { busy = false; msg = '\u8BF7\u6C42\u5931\u8D25'; setUI(); });
  }

  var dragging = false, moved = false, sx = 0, sy = 0, bx = 0, by = 0;
  pill.addEventListener('pointerdown', function (e) {
    dragging = true; moved = false; sx = e.clientX; sy = e.clientY;
    var r = pill.getBoundingClientRect(); bx = r.left; by = r.top;
    pill.setPointerCapture(e.pointerId);
    e.preventDefault();
  });
  pill.addEventListener('pointermove', function (e) {
    if (!dragging) return;
    if (Math.abs(e.clientX - sx) > 4 || Math.abs(e.clientY - sy) > 4) moved = true;
    if (moved) {
      pill.style.left = Math.max(0, bx + e.clientX - sx) + 'px';
      pill.style.top = Math.max(0, by + e.clientY - sy) + 'px';
      pill.style.right = 'auto'; pill.style.bottom = 'auto';
    }
  });
  pill.addEventListener('pointerup', function (e) {
    if (!dragging) return;
    dragging = false;
    if (!moved) act(running ? '/bridge-launcher/stop' : '/bridge-launcher/start');
  });

  refresh();
  setInterval(refresh, 3000);
})();
`;

export const inject = [];

export const apply = (ctx) => {
  const webServer = ctx.get('webServer');
  if (webServer === undefined) return;

  let child = null;
  let alive = false;
  let lastLog = '';

  const start = () => {
    if (alive) return { ok: true, running: true, msg: 'already-running' };
    try {
      child = spawn(
        process.execPath,
        [bridgeScript, '--host', '0.0.0.0', '--port', '8765'],
        { cwd: repoRoot, stdio: 'ignore' }
      );
    } catch (e) {
      child = null;
      return { ok: false, running: false, msg: 'spawn failed: ' + String(e && e.message ? e.message : e) };
    }
    alive = true;
    const h = child;
    h.on('exit', (code) => {
      if (h !== child) return;
      alive = false;
      child = null;
      lastLog = code === 0 ? 'bridge exited' : 'bridge exited (code ' + code + ')';
    });
    h.on('error', (e) => {
      if (h !== child) return;
      alive = false;
      child = null;
      lastLog = 'spawn error: ' + String(e && e.message ? e.message : e);
    });
    return { ok: true, running: true, msg: 'started' };
  };

  const stop = () => {
    if (!child) return { ok: true, running: false, msg: 'not-running' };
    try { child.kill(); } catch (e) { /* noop */ }
    child = null;
    alive = false;
    return { ok: true, running: false, msg: 'stopped' };
  };

  const disposers = [
    webServer.register({ kind: 'exact', path: '/bridge-launcher/status', handler: (req, res) => json(res, { running: alive, log: lastLog }) }),
    webServer.register({ kind: 'exact', path: '/bridge-launcher/start', handler: (req, res) => json(res, start()) }),
    webServer.register({ kind: 'exact', path: '/bridge-launcher/stop', handler: (req, res) => json(res, stop()) }),
    webServer.register({
      kind: 'exact',
      path: '/bridge-launcher/button.js',
      handler: (req, res) => {
        res.writeHead(200, { 'content-type': 'text/javascript; charset=utf-8' });
        res.end(BUTTON_JS);
      },
    }),
    webServer.tapIndex((html) =>
      html.includes('/bridge-launcher/button.js')
        ? html
        : html.replace('</body>', '<script src="/bridge-launcher/button.js"></script></body>')
    ),
  ];

  ctx.effect(() => () => {
    for (const dispose of disposers) {
      try { dispose(); } catch (e) { /* noop */ }
    }
    if (child) {
      try { child.kill(); } catch (e) { /* noop */ }
    }
    child = null;
    alive = false;
  });
};
