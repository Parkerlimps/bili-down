/* BiliDown 前端逻辑：解析 → 选清晰度 → 提交任务 → 轮询进度 */

const $ = (id) => document.getElementById(id);

const state = {
  data: null,      // 最近一次解析结果
  qn: 0,
  codecid: 7,
  page: 1,
  sub: "",
  tasks: [],
  timer: null,
  loginTimer: null,
};

/* ---------------- 基础 ---------------- */

async function api(path, opts = {}) {
  const res = await fetch(path, {
    headers: { "Content-Type": "application/json" },
    ...opts,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok || data.error) throw new Error(data.error || `请求失败 ${res.status}`);
  return data;
}

let toastTimer = null;
function toast(msg) {
  const el = $("toast");
  el.textContent = msg;
  el.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove("show"), 2600);
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
  while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; }
  return `${b.toFixed(b < 10 && i > 1 ? 1 : 0)} ${u[i]}`;
};

const fmtSpeed = (b) => (b ? `${fmtSize(b)}/s` : "");

const fmtFps = (v) => {
  const n = parseFloat(v);
  return isFinite(n) ? `${Math.round(n)}fps` : "";
};

/* ---------------- 账号 ---------------- */

async function loadMe() {
  const me = await api("/api/me");
  const area = $("userArea");
  area.innerHTML = "";
  if (me.logged_in && me.user) {
    const img = document.createElement("img");
    img.className = "avatar";
    img.src = me.user.face || "";
    img.referrerPolicy = "no-referrer";
    const span = document.createElement("span");
    span.className = "uname";
    span.textContent = me.user.name + (me.user.vip ? " · 大会员" : "");
    const out = document.createElement("button");
    out.className = "ghost";
    out.textContent = "退出";
    out.onclick = async () => {
      await api("/api/logout", { method: "POST", body: "{}" });
      toast("已退出登录");
      loadMe();
    };
    area.append(img, span, out);
    $("heroHint").textContent = `已登录 ${me.user.name}，可获取账号权限内的最高清晰度`;
  } else {
    const btn = document.createElement("button");
    btn.className = "primary";
    btn.style.padding = "7px 18px";
    btn.textContent = "扫码登录";
    btn.onclick = openLogin;
    area.append(btn);
  }
  if (!me.ffmpeg) toast("未找到 ffmpeg，下载将无法合并，请检查 bin 目录");
}

function openLogin() {
  $("loginModal").classList.remove("hidden");
  $("qrImg").removeAttribute("src");
  $("qrTip").textContent = "正在获取二维码…";
  api("/api/login/qr").then((d) => {
    $("qrImg").src = d.image;
    $("qrTip").textContent = "请用哔哩哔哩手机客户端扫码";
    clearInterval(state.loginTimer);
    state.loginTimer = setInterval(async () => {
      try {
        const r = await api(`/api/login/poll?key=${encodeURIComponent(d.key)}`);
        if (r.state === "ok") {
          clearInterval(state.loginTimer);
          $("loginModal").classList.add("hidden");
          toast(`登录成功：${r.user?.name || ""}`);
          loadMe();
          if (state.data) doParse(true, state.page);
        } else if (r.state === "scanned") {
          $("qrTip").textContent = "已扫码，请在手机上确认登录";
        } else if (r.state === "expired") {
          clearInterval(state.loginTimer);
          $("qrTip").textContent = "二维码已过期，请关闭重开";
        }
      } catch (e) { /* 忽略单次失败 */ }
    }, 1600);
  }).catch((e) => {
    $("qrTip").textContent = "获取二维码失败：" + e.message;
  });
}

/* ---------------- 解析 ---------------- */

async function doParse(silent = false, pageOverride = null) {
  const url = $("urlInput").value.trim();
  if (!url) return toast("先粘贴一个视频链接");

  const btn = $("parseBtn");
  btn.disabled = true;
  btn.textContent = "解析中";
  try {
    const page = pageOverride !== null ? pageOverride : 0;
    const d = await api(`/api/parse?url=${encodeURIComponent(url)}&p=${page}`);
    state.data = d;
    state.page = d.page;
    state.sub = "";
    renderResult(d);
    if (!silent) $("resultCard").scrollIntoView({ behavior: "smooth", block: "nearest" });
  } catch (e) {
    toast(e.message);
  } finally {
    btn.disabled = false;
    btn.textContent = "解析";
  }
}

function renderResult(d) {
  const v = d.video;
  $("resultCard").classList.remove("hidden");
  $("cover").src = v.cover || "";
  $("cover").referrerPolicy = "no-referrer";
  $("vTitle").textContent = v.title;
  $("vMeta").textContent =
    `${v.owner || "未知 UP"} · ${fmtDur(v.duration)} · ${v.stat.view ? (v.stat.view / 10000).toFixed(1) + " 万播放" : ""}`;
  $("vDesc").textContent = v.desc || "";
  $("vDesc").classList.toggle("hidden", !v.desc || v.desc === v.title);

  // 分 P
  const pageRow = $("pageRow");
  const sel = $("pageSel");
  if (v.pages.length > 1) {
    pageRow.classList.remove("hidden");
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

  // 警告
  const warn = $("warnBar");
  if (d.warn) {
    warn.classList.remove("hidden");
    warn.innerHTML = `${d.warn} <a id="warnLogin">立即扫码登录 →</a>`;
    $("warnLogin").onclick = openLogin;
  } else {
    warn.classList.add("hidden");
  }

  // 清晰度
  const qnChips = $("qnChips");
  qnChips.innerHTML = "";
  const qs = d.play.qualities || [];
  state.qn = qs.length ? qs[0].qn : 0;
  qs.forEach((q) => {
    const b = document.createElement("button");
    b.className = "chip" + (q.qn === state.qn ? " on" : "");
    b.dataset.qn = q.qn;
    const res = q.width && q.height ? ` ${q.height}P` : "";
    b.innerHTML = `${q.name}<span class="tag">${fmtFps(q.fps)}</span>`;
    b.onclick = () => {
      state.qn = q.qn;
      [...qnChips.children].forEach((c) => c.classList.toggle("on", +c.dataset.qn === q.qn));
      renderCodecs(q);
      updateSize();
    };
    qnChips.appendChild(b);
  });

  if (qs.length) renderCodecs(qs[0]);

  // 字幕
  const subRow = $("subRow");
  const subSel = $("subSel");
  subSel.innerHTML = '<option value="">不下载字幕</option>';
  if (d.subtitles && d.subtitles.length) {
    subRow.classList.remove("hidden");
    d.subtitles.forEach((s) => {
      const o = document.createElement("option");
      o.value = s.lan;
      o.textContent = s.label + (s.lan.startsWith("ai") ? "（AI 生成）" : "");
      subSel.appendChild(o);
    });
  } else {
    subRow.classList.add("hidden");
  }
  subSel.onchange = () => { state.sub = subSel.value; };

  updateSize();
}

function renderCodecs(q) {
  const box = $("codecChips");
  box.innerHTML = "";
  const list = q.codecs || [];
  const prefer = list.find((c) => c.codecid === 7) || list[0];
  if (!list.some((c) => c.codecid === state.codecid)) state.codecid = prefer ? prefer.codecid : 7;

  list.forEach((c) => {
    const b = document.createElement("button");
    b.className = "chip" + (c.codecid === state.codecid ? " on" : "");
    b.dataset.cc = c.codecid;
    b.textContent = c.name;
    b.onclick = () => {
      state.codecid = c.codecid;
      [...box.children].forEach((x) => x.classList.toggle("on", +x.dataset.cc === c.codecid));
      updateSize();
    };
    box.appendChild(b);
  });
}

function updateSize() {
  const d = state.data;
  if (!d) return;
  const q = (d.play.qualities || []).find((x) => x.qn === state.qn);
  const c = q?.codecs.find((x) => x.codecid === state.codecid) || q?.codecs[0];
  const audio = d.play.audio?.[0];
  const dur = (d.video.pages[d.page - 1] || {}).duration || d.video.duration || 0;
  let bytes = 0;
  if (c?.size) bytes += c.size;
  else if (c?.bandwidth) bytes += (c.bandwidth * dur) / 8;
  if (audio?.bandwidth) bytes += (audio.bandwidth * dur) / 8;
  $("sizeHint").textContent = bytes
    ? `预计 ${fmtSize(bytes)} · ${c?.name || ""} + ${audio?.name || "无"} 音轨 · ${fmtDur(dur)}`
    : "";
}

/* ---------------- 下载 ---------------- */

async function startDownload() {
  const d = state.data;
  if (!d) return;
  const q = (d.play.qualities || []).find((x) => x.qn === state.qn);
  const c = q?.codecs.find((x) => x.codecid === state.codecid) || q?.codecs[0];
  if (!c) return toast("没有可用的清晰度");

  const btn = $("dlBtn");
  btn.disabled = true;
  try {
    await api("/api/download", {
      method: "POST",
      body: JSON.stringify({
        bvid: d.video.bvid,
        cid: d.cid,
        title: d.video.title,
        part: (d.video.pages[d.page - 1] || {}).part || "",
        page: d.page,
        qn: q.qn,
        codecid: c.codecid,
        sub_lan: state.sub || null,
      }),
    });
    toast("已加入下载队列");
    pollTasks(true);
  } catch (e) {
    toast(e.message);
  } finally {
    btn.disabled = false;
  }
}

/* ---------------- 任务 ---------------- */

let renderedIds = "";

async function pollTasks(force = false) {
  try {
    const d = await api("/api/tasks");
    state.tasks = d.tasks;
    renderTasks();
    const active = d.tasks.some((t) => !["done", "error", "cancelled"].includes(t.status));
    clearTimeout(state.timer);
    if (active) state.timer = setTimeout(pollTasks, 700);
    else if (d.tasks.length) state.timer = setTimeout(pollTasks, 4000);
  } catch (e) {
    if (force) toast(e.message);
  }
}

const STATUS_TEXT = {
  queued: "排队中", waiting: "等待中", parsing: "解析中",
  downloading: "下载中", merging: "合并中",
  done: "已完成", error: "失败", cancelled: "已取消",
};

function renderTasks() {
  const list = $("taskList");
  const tasks = state.tasks;
  $("taskEmpty").classList.toggle("hidden", tasks.length > 0);

  const ids = tasks.map((t) => t.id).join(",");
  if (ids !== renderedIds) {
    renderedIds = ids;
    list.innerHTML = "";
    tasks.forEach((t) => list.appendChild(buildTask(t)));
  }

  tasks.forEach((t) => updateTask(t));
}

function buildTask(t) {
  const el = document.createElement("div");
  el.className = "task";
  el.dataset.id = t.id;
  el.innerHTML = `
    <div class="task-top">
      <div class="t-title"><span class="t-qn"></span><span class="t-name"></span></div>
      <div class="t-status"></div>
    </div>
    <div class="bar"><i></i></div>
    <div class="task-foot"><span class="t-info"></span><span class="acts"></span></div>`;
  el.querySelector(".t-name").textContent =
    t.page > 1 && t.part ? `${t.title} · P${t.page} ${t.part}` : t.title;
  return el;
}

function updateTask(t) {
  const el = document.querySelector(`.task[data-id="${t.id}"]`);
  if (!el) return;
  el.dataset.status = t.status;
  el.querySelector(".t-qn").textContent = t.qn_name;
  const st = el.querySelector(".t-status");
  st.textContent = STATUS_TEXT[t.status] || t.status;
  st.className = "t-status " + (t.status === "done" ? "done" : t.status === "error" ? "error" : "");
  el.querySelector(".bar i").style.width = `${Math.min(100, t.pct)}%`;

  const info = el.querySelector(".t-info");
  const acts = el.querySelector(".acts");

  if (t.status === "done") {
    info.textContent = `${fmtSize(t.downloaded)} · ${t.phase}`;
    acts.innerHTML = `<a data-a="file">播放</a><a data-a="dir">打开目录</a>`;
  } else if (t.status === "error") {
    info.textContent = t.error || "未知错误";
    acts.innerHTML = `<a data-a="dir">打开目录</a>`;
  } else if (t.status === "cancelled") {
    info.textContent = "已取消";
    acts.innerHTML = "";
  } else {
    const parts = [`${t.pct.toFixed(1)}%`, t.phase];
    if (t.speed) parts.push(fmtSpeed(t.speed));
    if (t.eta && t.eta < 36000) parts.push(`剩余 ${fmtDur(t.eta)}`);
    info.textContent = parts.join(" · ");
    acts.innerHTML = `<a data-a="cancel">取消</a>`;
  }

  acts.querySelectorAll("a").forEach((a) => {
    a.onclick = () => {
      const kind = a.dataset.a;
      if (kind === "cancel") {
        api("/api/cancel", { method: "POST", body: JSON.stringify({ id: t.id }) })
          .then(() => toast("正在取消"));
      } else if (kind === "file") {
        api("/api/open", { method: "POST", body: JSON.stringify({ file: t.file }) });
      } else if (kind === "dir") {
        api("/api/open", { method: "POST", body: JSON.stringify({ file: t.file }) });
      }
    };
  });
}

/* ---------------- 事件绑定 ---------------- */

$("parseBtn").onclick = () => doParse();
$("urlInput").addEventListener("keydown", (e) => {
  if (e.key === "Enter") doParse();
});
$("dlBtn").onclick = startDownload;
$("openDirBtn").onclick = () => api("/api/open", { method: "POST", body: "{}" });
$("closeLogin").onclick = () => {
  $("loginModal").classList.add("hidden");
  clearInterval(state.loginTimer);
};
$("pageSel").onchange = (e) => {
  state.page = +e.target.value;
  doParse(true, state.page);
};

loadMe().catch(() => {});
pollTasks();
