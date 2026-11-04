/* BiliDown 前端：通过 WebView2 消息桥调用 C++ 后端 */

const $ = (id) => document.getElementById(id);

const S = { data: null, qn: 0, codecid: 7, page: 1, sub: "", tasks: [], ids: "", timer: null, qrTimer: null };

/* ---------- RPC ---------- */

let seq = 0;
const pending = new Map();

if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener("message", (ev) => {
    const m = ev.data;
    const p = pending.get(m.id);
    if (!p) return;
    pending.delete(m.id);
    m.ok ? p.resolve(m.data) : p.reject(new Error(m.error || "请求失败"));
  });
}

function rpc(method, params = {}) {
  return new Promise((resolve, reject) => {
    const id = String(++seq);
    pending.set(id, { resolve, reject });
    try {
      window.chrome.webview.postMessage({ id, method, params });
    } catch (e) {
      pending.delete(id);
      return reject(new Error("界面与内核通信失败"));
    }
    setTimeout(() => {
      if (pending.has(id)) {
        pending.delete(id);
        reject(new Error("请求超时"));
      }
    }, 40000);
  });
}

/* ---------- 工具 ---------- */

let toastT = null;
function toast(msg) {
  const el = $("toast");
  el.textContent = msg;
  el.classList.add("show");
  clearTimeout(toastT);
  toastT = setTimeout(() => el.classList.remove("show"), 2600);
}

const fmtDur = (s) => {
  s = Math.round(s || 0);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
  return h ? `${h}:${String(m).padStart(2, "0")}:${String(x).padStart(2, "0")}`
           : `${m}:${String(x).padStart(2, "0")}`;
};

const fmtSize = (b) => {
  if (!b) return "—";
  const u = ["B", "KB", "MB", "GB"];
  let i = 0;
  while (b >= 1024 && i < 3) { b /= 1024; i++; }
  return `${b < 10 && i > 1 ? b.toFixed(1) : Math.round(b)} ${u[i]}`;
};

const fmtSpeed = (b) => (b ? `${fmtSize(b)}/s` : "");

const fmtView = (n) => {
  if (!n) return "";
  if (n >= 1e8) return `${(n / 1e8).toFixed(1)} 亿播放`;
  if (n >= 1e4) return `${(n / 1e4).toFixed(1)} 万播放`;
  return `${n} 播放`;
};

const fmtFps = (v) => {
  const n = parseFloat(v);
  return isFinite(n) && n > 0 ? `${Math.round(n)}fps` : "";
};

/* ---------- 账号 ---------- */

let me = null;

async function loadMe() {
  const d = await rpc("init");
  me = d;
  const box = $("userArea");
  box.innerHTML = "";
  if (d.loggedIn) {
    const img = document.createElement("img");
    img.className = "avatar";
    img.src = d.face || "";
    img.referrerPolicy = "no-referrer";
    const name = document.createElement("span");
    name.className = "uname";
    name.textContent = d.name || "";
    const out = document.createElement("button");
    out.className = "ghost";
    out.textContent = "退出";
    out.onclick = async () => {
      await rpc("logout");
      toast("已退出登录");
      loadMe();
    };
    box.append(img, name, out);
  } else {
    const btn = document.createElement("button");
    btn.className = "primary";
    btn.textContent = "扫码登录";
    btn.onclick = openLogin;
    box.append(btn);
  }
  if (!d.ffmpeg) toast("缺少 ffmpeg.exe，无法合并音视频");
}

function openLogin() {
  $("modal").classList.remove("hidden");
  $("qrBox").innerHTML = "";
  $("qrTip").textContent = "正在获取二维码…";

  rpc("login.qr").then((d) => {
    $("qrBox").innerHTML = "";
    new QRCode($("qrBox"), {
      text: d.url,
      width: 190,
      height: 190,
      correctLevel: QRCode.CorrectLevel.M,
    });
    $("qrTip").textContent = "请用哔哩哔哩手机客户端扫码";
    clearTimeout(S.qrTimer);
    // 链式轮询：上一次返回之后才排下一次。用 setInterval 的话请求一慢就会堆叠
    const tick = async () => {
      let stop = false;
      try {
        const r = await rpc("login.poll", { key: d.key });
        if (r.state === "ok") {
          stop = true;
          $("modal").classList.add("hidden");
          toast(`登录成功：${(r.user && r.user.name) || ""}`);
          loadMe();
          if (S.data) doParse(true, S.page);
        } else if (r.state === "scanned") {
          $("qrTip").textContent = "已扫码，请在手机上确认";
        } else if (r.state === "expired") {
          stop = true;
          $("qrTip").textContent = "二维码已过期，请关闭后重试";
        }
      } catch (e) { /* 单次失败忽略 */ }
      // 弹窗已被关掉就不要再排下一次，否则会一直空转
      if (!stop && !$("modal").classList.contains("hidden")) {
        S.qrTimer = setTimeout(tick, 1600);
      }
    };
    S.qrTimer = setTimeout(tick, 1600);
  }).catch((e) => {
    $("qrTip").textContent = "获取二维码失败：" + e.message;
  });
}

/* ---------- 解析 ---------- */

async function doParse(silent = false, pageOverride = null) {
  const url = $("urlInput").value.trim();
  if (!url) return toast("先粘贴一个视频链接");

  const btn = $("parseBtn");
  btn.disabled = true;
  btn.textContent = "解析中";
  try {
    const d = await rpc("parse", { url, page: pageOverride || 0 });
    S.data = d;
    S.page = d.page;
    S.sub = "";
    render(d);
    if (!silent) $("card").scrollIntoView({ behavior: "smooth", block: "nearest" });
  } catch (e) {
    toast(e.message);
  } finally {
    btn.disabled = false;
    btn.textContent = "解析";
  }
}

function render(d) {
  const v = d.video;
  $("card").classList.remove("hidden");
  $("cover").src = v.cover || "";
  $("title").textContent = v.title;
  $("meta").textContent =
    [v.owner || "未知 UP", fmtDur(v.duration), fmtView(v.view)].filter(Boolean).join(" · ");

  const desc = (v.desc || "").trim();
  const useless = !desc || desc === v.title || desc.length < 3 || /^[-—–\s.。·]+$/.test(desc);
  $("desc").textContent = useless ? "" : desc;
  $("desc").classList.toggle("hidden", useless);

  const pageRow = $("pageRow");
  if (v.pages.length > 1) {
    pageRow.classList.remove("hidden");
    const sel = $("pageSel");
    sel.innerHTML = "";
    v.pages.forEach((p) => {
      const o = document.createElement("option");
      o.value = p.page;
      o.textContent = `P${p.page} ${p.part}（${fmtDur(p.duration)}）`;
      o.selected = p.page === d.page;
      sel.appendChild(o);
    });
  } else {
    pageRow.classList.add("hidden");
  }

  const warn = $("warn");
  if (d.warn) {
    warn.classList.remove("hidden");
    warn.innerHTML = `${d.warn} <a id="warnLogin">立即登录 →</a>`;
    $("warnLogin").onclick = openLogin;
  } else {
    warn.classList.add("hidden");
  }

  const qs = d.play.qualities || [];
  const chips = $("qnChips");
  chips.innerHTML = "";
  S.qn = qs.length ? qs[0].qn : 0;
  qs.forEach((q) => {
    const b = document.createElement("button");
    b.className = "chip" + (q.qn === S.qn ? " on" : "");
    b.dataset.qn = q.qn;
    b.innerHTML = `${q.name}<span class="t">${fmtFps(q.fps)}</span>`;
    b.onclick = () => {
      S.qn = q.qn;
      [...chips.children].forEach((c) => c.classList.toggle("on", +c.dataset.qn === q.qn));
      renderCodecs(q);
      updateEst();
    };
    chips.appendChild(b);
  });
  if (qs.length) renderCodecs(qs[0]);

  const subRow = $("subRow");
  const subSel = $("subSel");
  subSel.innerHTML = '<option value="">不下载字幕</option>';
  if (d.subtitles && d.subtitles.length) {
    subRow.classList.remove("hidden");
    d.subtitles.forEach((s) => {
      const o = document.createElement("option");
      o.value = s.lan;
      o.textContent = s.label + (s.lan.indexOf("ai") === 0 ? "（AI）" : "");
      subSel.appendChild(o);
    });
  } else {
    subRow.classList.add("hidden");
  }
  subSel.onchange = () => { S.sub = subSel.value; };

  updateEst();
}

function renderCodecs(q) {
  const box = $("codecChips");
  box.innerHTML = "";
  const list = q.codecs || [];
  const prefer = list.find((c) => c.codecid === 7) || list[0];
  if (!list.some((c) => c.codecid === S.codecid)) S.codecid = prefer ? prefer.codecid : 7;

  list.forEach((c) => {
    const b = document.createElement("button");
    b.className = "chip" + (c.codecid === S.codecid ? " on" : "");
    b.dataset.cc = c.codecid;
    b.textContent = c.name;
    b.onclick = () => {
      S.codecid = c.codecid;
      [...box.children].forEach((x) => x.classList.toggle("on", +x.dataset.cc === c.codecid));
      updateEst();
    };
    box.appendChild(b);
  });
}

function updateEst() {
  const d = S.data;
  if (!d) return;
  const q = (d.play.qualities || []).find((x) => x.qn === S.qn);
  const c = q && (q.codecs.find((x) => x.codecid === S.codecid) || q.codecs[0]);
  const a = (d.play.audio || [])[0];
  const dur = (d.video.pages[d.page - 1] || {}).duration || d.video.duration || 0;
  let bytes = 0;
  if (c && c.size) bytes += c.size;
  else if (c && c.bandwidth) bytes += (c.bandwidth * dur) / 8;
  if (a && a.bandwidth) bytes += (a.bandwidth * dur) / 8;
  $("est").textContent = bytes
    ? `约 ${fmtSize(bytes)} · ${c ? c.name : ""} · ${fmtDur(dur)}`
    : "";
}

/* ---------- 下载 ---------- */

async function startDownload() {
  const d = S.data;
  if (!d) return;
  const q = (d.play.qualities || []).find((x) => x.qn === S.qn);
  const c = q && (q.codecs.find((x) => x.codecid === S.codecid) || q.codecs[0]);
  if (!c) return toast("没有可用的清晰度");

  const btn = $("dlBtn");
  btn.disabled = true;
  try {
    await rpc("download", {
      bvid: d.video.bvid,
      cid: d.cid,
      title: d.video.title,
      part: (d.video.pages[d.page - 1] || {}).part || "",
      page: d.page,
      qn: q.qn,
      codecid: c.codecid,
      subLan: S.sub || "",
    });
    toast("已加入下载队列");
    poll(true);
  } catch (e) {
    toast(e.message);
  } finally {
    btn.disabled = false;
  }
}

/* ---------- 任务 ---------- */

const STATE = {
  queued: "排队中", parsing: "解析中", downloading: "下载中",
  merging: "合并中", done: "已完成", error: "失败", cancelled: "已取消",
};

async function poll(force = false) {
  try {
    const d = await rpc("tasks");
    S.tasks = d.tasks || [];
    renderTasks();
    const active = S.tasks.some((t) => !["done", "error", "cancelled"].includes(t.status));
    clearTimeout(S.timer);
    if (active) S.timer = setTimeout(poll, 600);
    else if (S.tasks.length) S.timer = setTimeout(poll, 3000);
  } catch (e) {
    if (force) toast(e.message);
  }
}

function renderTasks() {
  const list = $("taskList");
  const ids = S.tasks.map((t) => t.id).join(",");
  $("empty").classList.toggle("hidden", S.tasks.length > 0);

  if (ids !== S.ids) {
    S.ids = ids;
    list.innerHTML = "";
    S.tasks.forEach((t) => {
      const el = document.createElement("div");
      el.className = "task";
      el.dataset.id = t.id;
      el.innerHTML = `
        <div class="task-top">
          <div class="t-name"><span class="t-badge"></span><span class="t-title"></span></div>
          <div class="t-state"></div>
        </div>
        <div class="bar"><i></i></div>
        <div class="task-foot"><span class="t-info"></span><span class="acts"></span></div>`;
      el.querySelector(".t-badge").textContent = t.qnName;
      el.querySelector(".t-title").textContent =
        t.page > 1 && t.part ? `${t.title} · P${t.page}` : t.title;
      list.appendChild(el);
    });
  }

  S.tasks.forEach((t) => {
    const el = list.querySelector(`.task[data-id="${t.id}"]`);
    if (!el) return;
    el.dataset.s = t.status;
    const st = el.querySelector(".t-state");
    st.textContent = STATE[t.status] || t.status;
    st.className = "t-state" + (t.status === "done" ? " done" : t.status === "error" ? " error" : "");
    el.querySelector(".bar i").style.width = `${Math.min(100, t.pct)}%`;

    const info = el.querySelector(".t-info");
    const acts = el.querySelector(".acts");

    if (t.status === "done") {
      info.textContent = `${fmtSize(t.done)} · ${t.phase}`;
      acts.innerHTML = `<a data-a="play">播放</a><a data-a="dir">打开目录</a>`;
    } else if (t.status === "error") {
      info.textContent = t.error || "未知错误";
      acts.innerHTML = `<a data-a="dir">打开目录</a>`;
    } else if (t.status === "cancelled") {
      info.textContent = "已取消";
      acts.innerHTML = "";
    } else {
      const parts = [`${t.pct.toFixed(0)}%`, t.phase];
      if (t.speed) parts.push(fmtSpeed(t.speed));
      info.textContent = parts.join(" · ");
      acts.innerHTML = `<a data-a="cancel">取消</a>`;
    }

    acts.querySelectorAll("a").forEach((a) => {
      a.onclick = () => {
        const k = a.dataset.a;
        if (k === "cancel") rpc("cancel", { id: t.id }).catch(() => {});
        else if (k === "play" || k === "dir") rpc("open", { file: t.file }).catch(() => {});
      };
    });
  });
}

/* ---------- 绑定 ---------- */

$("parseBtn").onclick = () => doParse();
$("urlInput").addEventListener("keydown", (e) => { if (e.key === "Enter") doParse(); });
$("dlBtn").onclick = startDownload;
$("openDir").onclick = () => rpc("open", { file: "" }).catch(() => {});
$("closeModal").onclick = () => {
  $("modal").classList.add("hidden");
  clearTimeout(S.qrTimer);
};
$("pageSel").onchange = (e) => {
  S.page = +e.target.value;
  doParse(true, S.page);
};

loadMe().catch((e) => toast(e.message));
poll();
