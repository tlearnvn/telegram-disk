/* =============================================================================
   Tuấn's Telegram Disk — giao diện web
   Thiết kế bởi Tuandethuong.
   ========================================================================== */
'use strict';

/* ── Tiện ích ngắn gọn ─────────────────────────────────────────────────── */
const $  = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'html') node.innerHTML = v;
    else if (k === 'text') node.textContent = v;
    else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
    else if (k === 'dataset') Object.assign(node.dataset, v);
    else node.setAttribute(k, v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    node.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
  }
  return node;
}

const KB = 1024, MB = KB * 1024, GB = MB * 1024, TB = GB * 1024;
function dungLuong(n) {
  n = Number(n) || 0;
  if (n < KB) return n + ' B';
  const [v, u] = n < MB ? [n / KB, 'KB'] : n < GB ? [n / MB, 'MB']
               : n < TB ? [n / GB, 'GB'] : [n / TB, 'TB'];
  return v.toFixed(v >= 100 ? 0 : v >= 10 ? 1 : 2).replace('.', ',') + ' ' + u;
}
function tocDo(bps) { return dungLuong(bps) + '/s'; }
function thoiLuong(giay) {
  giay = Math.max(0, Math.round(giay));
  if (giay < 60) return giay + ' giây';
  if (giay < 3600) {
    const p = Math.floor(giay / 60), s = giay % 60;
    return s ? `${p} phút ${s} giây` : `${p} phút`;
  }
  if (giay < 86400) {
    const h = Math.floor(giay / 3600), p = Math.floor((giay % 3600) / 60);
    return p ? `${h} giờ ${p} phút` : `${h} giờ`;
  }
  const d = Math.floor(giay / 86400), h = Math.floor((giay % 86400) / 3600);
  return h ? `${d} ngày ${h} giờ` : `${d} ngày`;
}
// Toàn hệ thống dùng UTC+7.
function ngayGio(epochGiay) {
  if (!epochGiay) return '—';
  const d = new Date((Number(epochGiay) + 7 * 3600) * 1000);
  const p = (n) => String(n).padStart(2, '0');
  return `${p(d.getUTCDate())}/${p(d.getUTCMonth() + 1)}/${d.getUTCFullYear()} ` +
         `${p(d.getUTCHours())}:${p(d.getUTCMinutes())}`;
}
function ghepDuongDan(a, b) {
  if (!a || a === '/') return '/' + b;
  return a.replace(/\/+$/, '') + '/' + b;
}
function thuMucCha(p) {
  const n = (p || '/').replace(/\/+$/, '');
  const i = n.lastIndexOf('/');
  return i <= 0 ? '/' : n.slice(0, i);
}
function thoat(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

/* ── SHA-256 (dự phòng khi trình duyệt không cho dùng crypto.subtle) ────── */
const Sha256 = (() => {
  const K = new Uint32Array([
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
  const rr = (x, n) => (x >>> n) | (x << (32 - n));

  return function hash(bytes) {
    let h = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                             0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
    const len = bytes.length;
    const padded = new Uint8Array((((len + 8) >> 6) + 1) * 64);
    padded.set(bytes);
    padded[len] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 4, (len << 3) >>> 0);
    view.setUint32(padded.length - 8, Math.floor(len / 536870912));

    const w = new Uint32Array(64);
    for (let i = 0; i < padded.length; i += 64) {
      for (let j = 0; j < 16; ++j) w[j] = view.getUint32(i + j * 4);
      for (let j = 16; j < 64; ++j) {
        const s0 = rr(w[j-15],7) ^ rr(w[j-15],18) ^ (w[j-15] >>> 3);
        const s1 = rr(w[j-2],17) ^ rr(w[j-2],19) ^ (w[j-2] >>> 10);
        w[j] = (w[j-16] + s0 + w[j-7] + s1) >>> 0;
      }
      let [a,b,c,d,e,f,g,hh] = h;
      for (let j = 0; j < 64; ++j) {
        const S1 = rr(e,6) ^ rr(e,11) ^ rr(e,25);
        const ch = (e & f) ^ (~e & g);
        const t1 = (hh + S1 + ch + K[j] + w[j]) >>> 0;
        const S0 = rr(a,2) ^ rr(a,13) ^ rr(a,22);
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (S0 + maj) >>> 0;
        hh = g; g = f; f = e; e = (d + t1) >>> 0;
        d = c; c = b; b = a; a = (t1 + t2) >>> 0;
      }
      h[0]=(h[0]+a)>>>0; h[1]=(h[1]+b)>>>0; h[2]=(h[2]+c)>>>0; h[3]=(h[3]+d)>>>0;
      h[4]=(h[4]+e)>>>0; h[5]=(h[5]+f)>>>0; h[6]=(h[6]+g)>>>0; h[7]=(h[7]+hh)>>>0;
    }
    let out = '';
    for (let i = 0; i < 8; ++i) out += h[i].toString(16).padStart(8, '0');
    return out;
  };
})();

async function bamNhanh(file) {
  // Băm nhanh = SHA-256 của (1 MB đầu ‖ 1 MB cuối ‖ kích thước) — đủ để đoán
  // tệp trùng mà không phải đọc hết tệp lớn.
  const mau = 1024 * 1024;
  const phan = [];
  phan.push(await file.slice(0, Math.min(mau, file.size)).arrayBuffer());
  if (file.size > mau * 2) phan.push(await file.slice(file.size - mau).arrayBuffer());
  const kichThuoc = new TextEncoder().encode(String(file.size));
  let tong = kichThuoc.length;
  for (const p of phan) tong += p.byteLength;
  const buf = new Uint8Array(tong);
  let off = 0;
  for (const p of phan) { buf.set(new Uint8Array(p), off); off += p.byteLength; }
  buf.set(kichThuoc, off);

  if (window.crypto && crypto.subtle && crypto.subtle.digest) {
    try {
      const digest = await crypto.subtle.digest('SHA-256', buf);
      return Array.from(new Uint8Array(digest))
        .map((b) => b.toString(16).padStart(2, '0')).join('');
    } catch (e) { /* rơi về bản thuần JS bên dưới */ }
  }
  return Sha256(buf);
}

/* ── Gọi API ───────────────────────────────────────────────────────────── */
async function api(duongDan, tuyChon = {}) {
  const opts = Object.assign({ credentials: 'same-origin', headers: {} }, tuyChon);
  if (opts.body !== undefined && !(opts.body instanceof Blob) &&
      !(opts.body instanceof ArrayBuffer) && typeof opts.body !== 'string') {
    opts.body = JSON.stringify(opts.body);
    opts.headers['Content-Type'] = 'application/json';
  }
  const res = await fetch(duongDan, opts);
  let data = null;
  const ct = res.headers.get('Content-Type') || '';
  if (ct.includes('json')) {
    try { data = await res.json(); } catch (e) { data = null; }
  }
  if (!res.ok) {
    const msg = (data && data.error) || `Lỗi máy chủ (${res.status})`;
    const err = new Error(msg);
    err.status = res.status;
    err.data = data;
    throw err;
  }
  return data;
}

/* ── Thông báo nổi ─────────────────────────────────────────────────────── */
function thongBao(tieuDe, noiDung = '', loai = 'info', giay = 5) {
  const khay = $('#khay-thong-bao');
  const icon = { ok: '✓', err: '✕', warn: '!', info: 'i' }[loai] || 'i';
  const node = el('div', { class: 'toast ' + loai },
    el('div', { class: 't-body' },
      el('div', { class: 't-title' }, `${icon}  ${tieuDe}`),
      noiDung ? el('div', { class: 't-msg', text: noiDung }) : null),
    el('button', { class: 't-close', 'aria-label': 'Đóng', onclick: () => node.remove() }, '✕'));
  khay.appendChild(node);
  if (giay > 0) setTimeout(() => node.remove(), giay * 1000);
  return node;
}

/* ── Hộp thoại ─────────────────────────────────────────────────────────── */
const HopThoai = {
  mo(tieuDe, thanNode, nutList, rong = false) {
    $('#tieu-de-hop-thoai').textContent = tieuDe;
    const than = $('#than-hop-thoai');
    than.innerHTML = '';
    than.appendChild(thanNode);
    const chan = $('#chan-hop-thoai');
    chan.innerHTML = '';
    for (const n of nutList) chan.appendChild(n);
    $('#hop-thoai').classList.toggle('wide', rong);
    $('#goc-hop-thoai').hidden = false;
    const focusable = than.querySelector('input, select, textarea, button');
    if (focusable) setTimeout(() => focusable.focus(), 40);
  },
  dong() { $('#goc-hop-thoai').hidden = true; },

  xacNhan(tieuDe, thongDiep, nhanDongY = 'Đồng ý', nguyHiem = false) {
    return new Promise((resolve) => {
      const than = el('div', {}, el('p', { text: thongDiep, style: 'margin:0;line-height:1.6' }));
      const huy = el('button', { class: 'btn btn-ghost',
        onclick: () => { HopThoai.dong(); resolve(false); } }, 'Huỷ');
      const ok = el('button', { class: 'btn ' + (nguyHiem ? 'btn-ghost danger' : 'btn-primary'),
        onclick: () => { HopThoai.dong(); resolve(true); } }, nhanDongY);
      HopThoai.mo(tieuDe, than, [huy, ok]);
    });
  },

  nhap(tieuDe, nhan, giaTri = '', nhanDongY = 'Lưu') {
    return new Promise((resolve) => {
      const input = el('input', { class: 'input', value: giaTri, style: 'width:100%' });
      const than = el('div', { class: 'setting' }, el('label', { text: nhan }), input);
      const huy = el('button', { class: 'btn btn-ghost',
        onclick: () => { HopThoai.dong(); resolve(null); } }, 'Huỷ');
      const ok = el('button', { class: 'btn btn-primary',
        onclick: () => { HopThoai.dong(); resolve(input.value); } }, nhanDongY);
      input.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); HopThoai.dong(); resolve(input.value); }
      });
      HopThoai.mo(tieuDe, than, [huy, ok]);
      setTimeout(() => { input.focus(); input.select(); }, 50);
    });
  },
};

/* ── Trạng thái ứng dụng ───────────────────────────────────────────────── */
const S = {
  nguoiDung: null,
  duongDan: '/',
  cheDo: localStorage.getItem('ttd-che-do') || 'grid',
  sapXep: localStorage.getItem('ttd-sap-xep') || 'name',
  giamDan: localStorage.getItem('ttd-giam-dan') === '1',
  timKiem: '',
  trang: 0,
  soMoiTrang: 200,
  muc: [],
  daChon: new Set(),
  view: 'tep',
  phienTaiLen: new Map(),   // id -> đối tượng theo dõi
  nhatKySse: null,
  phienBan: null,
  hesoTaiLen: { chunk: 8 * 1024 * 1024 },
};

/* ── Biểu tượng theo loại ──────────────────────────────────────────────── */
const BIEU_TUONG = {
  folder: '📁', image: '🖼️', video: '🎬', audio: '🎵', pdf: '📕',
  archive: '🗜️', code: '💻', document: '📄', spreadsheet: '📊',
  presentation: '📽️', text: '📝', font: '🔤', other: '📦',
};
function loaiCuaMuc(m) { return m.is_folder ? 'folder' : (m.category || 'other'); }

/* ══════════════════════════════════════════════════════════════════════════
   Đăng nhập
   ══════════════════════════════════════════════════════════════════════════ */
async function khoiDong() {
  apDungGiaoDien(localStorage.getItem('ttd-giao-dien') || 'auto');
  try {
    const v = await api('/api/version');
    S.phienBan = v;
    $('#pb-ten').textContent = v.app;
    $('#pb-so').textContent = 'v' + v.version + ' · b' + v.build;
    $('#pb-so').title = `Phiên bản ${v.version}, build ${v.build}\n` +
                        `Commit ${v.commit} (${v.branch})\nBiên dịch: ${v.build_time}`;
    document.title = v.app;
  } catch (e) { /* không sao, vẫn tiếp tục */ }

  try {
    const me = await api('/api/auth/me');
    vaoUngDung(me.user);
  } catch (e) {
    hienDangNhap();
  }
}

function hienDangNhap() {
  $('#man-hinh-dang-nhap').classList.remove('hidden');
  $('#ung-dung').classList.add('hidden');
  setTimeout(() => $('#dn-ten').focus(), 60);
}

function vaoUngDung(nguoiDung) {
  S.nguoiDung = nguoiDung;
  $('#man-hinh-dang-nhap').classList.add('hidden');
  $('#ung-dung').classList.remove('hidden');

  $('#ten-hien-thi').textContent = nguoiDung.display_name || nguoiDung.username;
  $('#menu-ten').textContent = nguoiDung.display_name || nguoiDung.username;
  $('#menu-vaitro').textContent = nguoiDung.is_admin ? 'Quản trị viên' : 'Người dùng';
  $('#chu-cai-dau').textContent =
    (nguoiDung.display_name || nguoiDung.username || '?').trim().charAt(0).toUpperCase();

  for (const n of $$('.admin-only')) n.style.display = nguoiDung.is_admin ? '' : 'none';

  $('#chon-sap-xep').value = S.sapXep;
  datCheDoHienThi(S.cheDo);
  chuyenView('tep');
  napDanhSach();
  capNhatThongKeNhanh();
  setInterval(capNhatThongKeNhanh, 20000);
  setInterval(dongBoTaiLen, 2000);
}

$('#form-dang-nhap').addEventListener('submit', async (e) => {
  e.preventDefault();
  const nut = $('#dn-nut');
  const loi = $('#dn-loi');
  loi.hidden = true;
  nut.disabled = true;
  nut.textContent = 'Đang đăng nhập…';
  try {
    const kq = await api('/api/auth/login', {
      method: 'POST',
      body: { username: $('#dn-ten').value.trim(), password: $('#dn-matkhau').value },
    });
    vaoUngDung(kq.user);
    thongBao('Xin chào ' + (kq.user.display_name || kq.user.username), '', 'ok', 3);
  } catch (err) {
    loi.textContent = err.message;
    loi.hidden = false;
  } finally {
    nut.disabled = false;
    nut.textContent = 'Đăng nhập';
  }
});

$('#dn-hien').addEventListener('click', () => {
  const i = $('#dn-matkhau');
  i.type = i.type === 'password' ? 'text' : 'password';
});

/* ══════════════════════════════════════════════════════════════════════════
   Giao diện sáng/tối
   ══════════════════════════════════════════════════════════════════════════ */
function apDungGiaoDien(che) {
  let thuc = che;
  if (che === 'auto') {
    thuc = window.matchMedia && matchMedia('(prefers-color-scheme: dark)').matches
      ? 'dark' : 'light';
  }
  document.documentElement.setAttribute('data-theme', thuc);
  localStorage.setItem('ttd-giao-dien', che);
  const ico = $('#ico-giao-dien');
  if (ico) {
    ico.innerHTML = thuc === 'dark'
      ? '<circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M4.9 4.9l1.4 1.4m11.4 11.4l1.4 1.4M2 12h2m16 0h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>'
      : '<path d="M21 12.8A9 9 0 1111.2 3a7 7 0 009.8 9.8z"/>';
  }
}
$('#nut-doi-giao-dien').addEventListener('click', () => {
  const hienTai = document.documentElement.getAttribute('data-theme');
  apDungGiaoDien(hienTai === 'dark' ? 'light' : 'dark');
});

/* ══════════════════════════════════════════════════════════════════════════
   Điều hướng
   ══════════════════════════════════════════════════════════════════════════ */
function chuyenView(ten) {
  S.view = ten;
  for (const a of $$('.nav-item')) a.classList.toggle('active', a.dataset.view === ten);
  for (const v of $$('.view')) v.classList.add('hidden');
  const map = {
    tep: 'view-tep', 'danh-dau': 'view-tep', 'tai-len': 'view-tai-len',
    'thung-rac': 'view-thung-rac', 'thong-ke': 'view-thong-ke',
    'nhat-ky': 'view-nhat-ky', 'tai-khoan': 'view-tai-khoan',
    'nguoi-dung': 'view-nguoi-dung', 'cai-dat': 'view-cai-dat',
  };
  const id = map[ten] || 'view-tep';
  $('#' + id).classList.remove('hidden');
  $('#thanh-cong-cu').style.display = (ten === 'tep' || ten === 'danh-dau') ? '' : 'none';
  $('#duong-dan').style.display = (ten === 'tep' || ten === 'danh-dau') ? '' : 'none';

  if (ten === 'thung-rac') napThungRac();
  else if (ten === 'thong-ke') napThongKe();
  else if (ten === 'nhat-ky') batNhatKy();
  else if (ten === 'tai-khoan') napTaiKhoan();
  else if (ten === 'nguoi-dung') napNguoiDung();
  else if (ten === 'cai-dat') napCaiDat();
  else if (ten === 'tai-len') veDanhSachTaiLen();
  else if (ten === 'danh-dau') napDanhSach();
  else napDanhSach();

  if (ten !== 'nhat-ky') tatNhatKy();
  dongThanhBen();
}

for (const a of $$('.nav-item')) {
  a.addEventListener('click', (e) => { e.preventDefault(); chuyenView(a.dataset.view); });
}

function moThanhBen() {
  $('#thanh-ben').classList.add('open');
  $('#lop-phu-thanh-ben').classList.add('show');
}
function dongThanhBen() {
  $('#thanh-ben').classList.remove('open');
  $('#lop-phu-thanh-ben').classList.remove('show');
}
$('#nut-mo-thanh-ben').addEventListener('click', moThanhBen);
$('#nut-dong-thanh-ben').addEventListener('click', dongThanhBen);
$('#lop-phu-thanh-ben').addEventListener('click', dongThanhBen);

/* ══════════════════════════════════════════════════════════════════════════
   Danh sách tệp
   ══════════════════════════════════════════════════════════════════════════ */
function datCheDoHienThi(che) {
  S.cheDo = che;
  localStorage.setItem('ttd-che-do', che);
  $('#nut-che-do-luoi').classList.toggle('active', che === 'grid');
  $('#nut-che-do-danh-sach').classList.toggle('active', che === 'list');
  const ds = $('#danh-sach-tep');
  ds.className = 'files ' + (che === 'grid' ? 'grid' : 'list');
}
$('#nut-che-do-luoi').addEventListener('click', () => { datCheDoHienThi('grid'); veDanhSach(); });
$('#nut-che-do-danh-sach').addEventListener('click', () => { datCheDoHienThi('list'); veDanhSach(); });

$('#chon-sap-xep').addEventListener('change', (e) => {
  S.sapXep = e.target.value;
  localStorage.setItem('ttd-sap-xep', S.sapXep);
  napDanhSach();
});
$('#nut-thu-tu').addEventListener('click', () => {
  S.giamDan = !S.giamDan;
  localStorage.setItem('ttd-giam-dan', S.giamDan ? '1' : '0');
  napDanhSach();
});

let timerTim = null;
$('#o-tim-kiem').addEventListener('input', (e) => {
  clearTimeout(timerTim);
  timerTim = setTimeout(() => {
    S.timKiem = e.target.value.trim();
    S.trang = 0;
    napDanhSach();
  }, 260);
});

async function napDanhSach() {
  if (S.view !== 'tep' && S.view !== 'danh-dau') return;
  const params = new URLSearchParams({
    path: S.duongDan,
    sort: S.sapXep,
    desc: S.giamDan ? '1' : '0',
    limit: String(S.soMoiTrang),
    offset: String(S.trang * S.soMoiTrang),
  });
  if (S.timKiem) params.set('search', S.timKiem);
  if (S.view === 'danh-dau') params.set('starred', '1');

  try {
    const kq = await api('/api/files?' + params.toString());
    S.muc = kq.entries || [];
    S.tong = kq.total || 0;
    S.daChon.clear();
    veDuongDan(kq.breadcrumb || []);
    veDanhSach();
    capNhatChon();
  } catch (err) {
    thongBao('Không tải được danh sách', err.message, 'err');
  }
}

function veDuongDan(crumbs) {
  const box = $('#duong-dan');
  box.innerHTML = '';
  if (S.view === 'danh-dau') {
    box.appendChild(el('a', { class: 'current', href: '#' }, 'Đánh dấu sao'));
    return;
  }
  if (S.timKiem) {
    box.appendChild(el('a', {
      href: '#', onclick: (e) => { e.preventDefault(); $('#o-tim-kiem').value = '';
        S.timKiem = ''; napDanhSach(); } }, '← Thoát tìm kiếm'));
    box.appendChild(el('i', {}, '/'));
    box.appendChild(el('a', { class: 'current', href: '#' }, `Kết quả cho “${S.timKiem}”`));
    return;
  }
  crumbs.forEach((c, i) => {
    if (i > 0) box.appendChild(el('i', {}, '›'));
    const cuoi = i === crumbs.length - 1;
    box.appendChild(el('a', {
      href: '#',
      class: cuoi ? 'current' : '',
      title: c.path,
      onclick: (e) => { e.preventDefault(); moThuMuc(c.path); },
    }, c.name || 'Ổ đĩa của tôi'));
  });
}

function moThuMuc(duongDan) {
  S.duongDan = duongDan || '/';
  S.trang = 0;
  S.timKiem = '';
  $('#o-tim-kiem').value = '';
  if (S.view !== 'tep') { chuyenView('tep'); return; }
  napDanhSach();
}

function veDanhSach() {
  const box = $('#danh-sach-tep');
  box.innerHTML = '';
  const trong = S.muc.length === 0;
  $('#trong-rong').hidden = !trong;
  $('#danh-sach-tep').hidden = trong;

  if (!trong && S.cheDo === 'list') {
    box.appendChild(el('div', { class: 'list-head' },
      el('span', {}, ''), el('span', {}, 'Tên'), el('span', {}, 'Kích thước'),
      el('span', { class: 'h-date' }, 'Ngày sửa'), el('span', {}, '')));
  }

  for (const m of S.muc) {
    box.appendChild(S.cheDo === 'grid' ? veThe(m) : veHang(m));
  }

  const soTrang = Math.max(1, Math.ceil((S.tong || 0) / S.soMoiTrang));
  $('#phan-trang').hidden = soTrang <= 1;
  $('#thong-tin-trang').textContent = `Trang ${S.trang + 1} / ${soTrang} · ${S.tong} mục`;
  $('#nut-trang-truoc').disabled = S.trang === 0;
  $('#nut-trang-sau').disabled = S.trang + 1 >= soTrang;
}

function huyHieu(m) {
  const list = [];
  if (m.starred) list.push(el('span', { class: 'pill star', title: 'Đã đánh dấu sao' }, '★'));
  if (m.shared) list.push(el('span', { class: 'pill share', title: 'Đang chia sẻ' }, '🔗'));
  return list.length ? el('div', { class: 'card-badges' }, list) : null;
}

function veThe(m) {
  const loai = loaiCuaMuc(m);
  const node = el('div', {
    class: 'card' + (S.daChon.has(m.id) ? ' selected' : ''),
    dataset: { id: String(m.id) },
    title: m.name,
  },
    huyHieu(m),
    el('div', { class: 'card-icon ft-' + loai }, BIEU_TUONG[loai] || BIEU_TUONG.other),
    el('div', { class: 'card-name', text: m.name }),
    el('div', { class: 'card-meta', text: m.is_folder ? 'Thư mục' : dungLuong(m.size) }));
  ganSuKienMuc(node, m);
  return node;
}

function veHang(m) {
  const loai = loaiCuaMuc(m);
  const node = el('div', {
    class: 'row-item' + (S.daChon.has(m.id) ? ' selected' : ''),
    dataset: { id: String(m.id) },
  },
    el('div', { class: 'row-icon ft-' + loai }, BIEU_TUONG[loai] || BIEU_TUONG.other),
    el('div', { class: 'row-name' },
      el('span', { text: m.name, style: 'overflow:hidden;text-overflow:ellipsis' }),
      m.starred ? el('span', { title: 'Đã đánh dấu sao' }, '★') : null,
      m.shared ? el('span', { title: 'Đang chia sẻ' }, '🔗') : null),
    el('div', { class: 'row-size', text: m.is_folder ? '—' : dungLuong(m.size) }),
    el('div', { class: 'row-date', text: ngayGio(m.modified_at) }),
    el('button', {
      class: 'icon-btn row-more', 'aria-label': 'Thao tác',
      onclick: (e) => { e.stopPropagation(); moMenuNguCanh(e, m); },
    }, '⋯'));
  ganSuKienMuc(node, m);
  return node;
}

function ganSuKienMuc(node, m) {
  node.addEventListener('click', (e) => {
    if (e.ctrlKey || e.metaKey) { toggleChon(m.id); return; }
    if (e.shiftKey) { chonDenMuc(m.id); return; }
    if (S.daChon.size > 0) { S.daChon.clear(); capNhatChon(); veDanhSach(); return; }
    if (m.is_folder) moThuMuc(m.path);
    else moXemTruoc(m);
  });
  node.addEventListener('dblclick', (e) => {
    e.preventDefault();
    if (m.is_folder) moThuMuc(m.path); else moXemTruoc(m);
  });
  node.addEventListener('contextmenu', (e) => { e.preventDefault(); moMenuNguCanh(e, m); });
}

function toggleChon(id) {
  if (S.daChon.has(id)) S.daChon.delete(id); else S.daChon.add(id);
  capNhatChon();
  veDanhSach();
}
function chonDenMuc(id) {
  const ids = S.muc.map((m) => m.id);
  const cuoi = ids.indexOf(id);
  const dauTien = ids.findIndex((x) => S.daChon.has(x));
  if (dauTien < 0) { S.daChon.add(id); }
  else {
    const [a, b] = dauTien < cuoi ? [dauTien, cuoi] : [cuoi, dauTien];
    for (let i = a; i <= b; ++i) S.daChon.add(ids[i]);
  }
  capNhatChon();
  veDanhSach();
}

function capNhatChon() {
  const n = S.daChon.size;
  $('#thong-tin-chon').hidden = n === 0;
  $('#hanh-dong-chon').hidden = n === 0;
  if (n) {
    let tong = 0;
    for (const m of S.muc) if (S.daChon.has(m.id) && !m.is_folder) tong += m.size;
    $('#thong-tin-chon').textContent =
      `Đã chọn ${n} mục` + (tong ? ` · ${dungLuong(tong)}` : '');
  }
}

$('#nut-trang-truoc').addEventListener('click', () => { if (S.trang > 0) { S.trang--; napDanhSach(); } });
$('#nut-trang-sau').addEventListener('click', () => { S.trang++; napDanhSach(); });

$('#hanh-dong-chon').addEventListener('click', async (e) => {
  const nut = e.target.closest('[data-op]');
  if (!nut) return;
  const ids = Array.from(S.daChon);
  const op = nut.dataset.op;
  if (op === 'bo-chon') { S.daChon.clear(); capNhatChon(); veDanhSach(); return; }
  if (op === 'tai-xuong') {
    for (const id of ids) {
      const m = S.muc.find((x) => x.id === id);
      if (m && !m.is_folder) taiXuong(m);
    }
    return;
  }
  if (op === 'xoa') {
    const ok = await HopThoai.xacNhan('Chuyển vào thùng rác',
      `Chuyển ${ids.length} mục vào thùng rác? Bạn vẫn có thể khôi phục sau.`,
      'Chuyển vào thùng rác', true);
    if (!ok) return;
    try {
      await api('/api/files/trash', { method: 'POST', body: { ids } });
      thongBao('Đã chuyển vào thùng rác', `${ids.length} mục`, 'ok', 3);
      napDanhSach(); capNhatThongKeNhanh();
    } catch (err) { thongBao('Không xoá được', err.message, 'err'); }
    return;
  }
  if (op === 'di-chuyen' || op === 'sao-chep') {
    const dich = await HopThoai.nhap(
      op === 'di-chuyen' ? 'Di chuyển tới' : 'Sao chép tới',
      'Đường dẫn thư mục đích (thư mục chưa có sẽ được tạo)',
      S.duongDan, op === 'di-chuyen' ? 'Di chuyển' : 'Sao chép');
    if (dich === null) return;
    try {
      await api(op === 'di-chuyen' ? '/api/files/move' : '/api/files/copy',
                { method: 'POST', body: { ids, target: dich } });
      thongBao('Hoàn tất', `${ids.length} mục → ${dich}`, 'ok', 3);
      napDanhSach();
    } catch (err) { thongBao('Thao tác thất bại', err.message, 'err'); }
  }
});

/* ── Menu ngữ cảnh ─────────────────────────────────────────────────────── */
function moMenuNguCanh(ev, m) {
  const menu = $('#menu-ngu-canh');
  menu.innerHTML = '';
  const them = (nhan, fn, lop = '') =>
    menu.appendChild(el('button', { class: lop, onclick: () => { dongMenu(); fn(); } }, nhan));

  if (m.is_folder) them('Mở thư mục', () => moThuMuc(m.path));
  else {
    them('Xem trước', () => moXemTruoc(m));
    them('Tải xuống', () => taiXuong(m));
  }
  them('Đổi tên', async () => {
    const ten = await HopThoai.nhap('Đổi tên', 'Tên mới', m.name, 'Đổi tên');
    if (ten === null || ten === m.name) return;
    try {
      await api('/api/files/rename', { method: 'POST', body: { id: m.id, name: ten } });
      napDanhSach();
      thongBao('Đã đổi tên', ten, 'ok', 3);
    } catch (err) { thongBao('Không đổi tên được', err.message, 'err'); }
  });
  them(m.starred ? 'Bỏ đánh dấu sao' : 'Đánh dấu sao', async () => {
    await api('/api/files/star', { method: 'POST', body: { id: m.id, starred: !m.starred } });
    napDanhSach();
  });
  them('Di chuyển…', async () => {
    const dich = await HopThoai.nhap('Di chuyển tới', 'Đường dẫn thư mục đích',
                                     thuMucCha(m.path), 'Di chuyển');
    if (dich === null) return;
    await api('/api/files/move', { method: 'POST', body: { ids: [m.id], target: dich } });
    napDanhSach();
  });
  them('Sao chép…', async () => {
    const dich = await HopThoai.nhap('Sao chép tới', 'Đường dẫn thư mục đích',
                                     S.duongDan, 'Sao chép');
    if (dich === null) return;
    await api('/api/files/copy', { method: 'POST', body: { ids: [m.id], target: dich } });
    napDanhSach();
  });
  menu.appendChild(el('hr'));
  them(m.shared ? 'Quản lý chia sẻ' : 'Tạo liên kết chia sẻ', () => hopThoaiChiaSe(m));
  them('Xem chi tiết', () => hopThoaiChiTiet(m));
  menu.appendChild(el('hr'));
  them('Chuyển vào thùng rác', async () => {
    await api('/api/files/trash', { method: 'POST', body: { ids: [m.id] } });
    thongBao('Đã chuyển vào thùng rác', m.name, 'ok', 3);
    napDanhSach(); capNhatThongKeNhanh();
  }, 'danger');

  menu.hidden = false;
  const w = menu.offsetWidth, h = menu.offsetHeight;
  let x = ev.clientX, y = ev.clientY;
  if (x + w > innerWidth - 8) x = innerWidth - w - 8;
  if (y + h > innerHeight - 8) y = innerHeight - h - 8;
  menu.style.left = Math.max(8, x) + 'px';
  menu.style.top = Math.max(8, y) + 'px';
}
function dongMenu() { $('#menu-ngu-canh').hidden = true; }
document.addEventListener('click', (e) => {
  if (!e.target.closest('#menu-ngu-canh')) dongMenu();
  if (!e.target.closest('.user-menu')) $('#menu-nguoi-dung').hidden = true;
});
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    dongMenu();
    if (!$('#goc-xem-truoc').hidden) dongXemTruoc();
    else if (!$('#goc-hop-thoai').hidden) HopThoai.dong();
  }
  if (e.key === 'a' && (e.ctrlKey || e.metaKey) &&
      (S.view === 'tep' || S.view === 'danh-dau') &&
      !['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement.tagName)) {
    e.preventDefault();
    for (const m of S.muc) S.daChon.add(m.id);
    capNhatChon(); veDanhSach();
  }
});

/* ── Tải xuống & xem trước ─────────────────────────────────────────────── */
function urlTai(m, taiVe) {
  return `/d/${m.id}/${encodeURIComponent(m.name)}` + (taiVe ? '?download=1' : '');
}
function taiXuong(m) {
  const a = el('a', { href: urlTai(m, true), download: m.name });
  document.body.appendChild(a);
  a.click();
  a.remove();
}

function moXemTruoc(m) {
  const goc = $('#goc-xem-truoc');
  const than = $('#than-xem-truoc');
  $('#xem-truoc-ten').textContent = m.name;
  $('#xem-truoc-tai').href = urlTai(m, true);
  $('#xem-truoc-tai').setAttribute('download', m.name);
  than.innerHTML = '';

  const url = urlTai(m, false);
  const loai = m.category;
  if (loai === 'image') {
    than.appendChild(el('img', { src: url, alt: m.name }));
  } else if (loai === 'video') {
    than.appendChild(el('video', { src: url, controls: 'controls', autoplay: 'autoplay',
                                   playsinline: 'playsinline' }));
  } else if (loai === 'audio') {
    than.appendChild(el('audio', { src: url, controls: 'controls', autoplay: 'autoplay' }));
  } else if (loai === 'pdf') {
    than.appendChild(el('iframe', { src: url, title: m.name }));
  } else if (loai === 'text' || loai === 'code') {
    const pre = el('pre', {}, 'Đang tải…');
    than.appendChild(pre);
    fetch(url, { credentials: 'same-origin', headers: { Range: 'bytes=0-262143' } })
      .then((r) => r.text())
      .then((t) => { pre.textContent = t + (m.size > 262144 ? '\n\n… (đã cắt bớt)' : ''); })
      .catch(() => { pre.textContent = 'Không đọc được nội dung.'; });
  } else {
    than.appendChild(el('div', { class: 'preview-note' },
      el('div', { style: 'font-size:52px' }, BIEU_TUONG[loai] || BIEU_TUONG.other),
      el('div', {}, `${m.name} · ${dungLuong(m.size)}`),
      el('div', { class: 'muted' }, 'Không xem trước được kiểu tệp này.'),
      el('a', { class: 'btn btn-primary', href: urlTai(m, true), download: m.name },
         'Tải xuống')));
  }
  goc.hidden = false;
}
function dongXemTruoc() {
  $('#than-xem-truoc').innerHTML = '';
  $('#goc-xem-truoc').hidden = true;
}
$('#nut-dong-xem-truoc').addEventListener('click', dongXemTruoc);

/* ── Chi tiết & chia sẻ ────────────────────────────────────────────────── */
async function hopThoaiChiTiet(m) {
  let chiTiet = null;
  try { chiTiet = await api('/api/file/' + m.id); } catch (e) { /* dùng dữ liệu sẵn có */ }
  const e2 = (chiTiet && chiTiet.entry) || m;
  const rows = [
    ['Tên', e2.name],
    ['Đường dẫn', e2.path],
    ['Loại', e2.is_folder ? 'Thư mục' : (e2.mime_type || 'Không rõ')],
    ['Kích thước', e2.is_folder
      ? (chiTiet ? dungLuong(chiTiet.folder_size) : '—') : dungLuong(e2.size)],
    ['Ngày tạo', ngayGio(e2.created_at)],
    ['Ngày sửa', ngayGio(e2.modified_at)],
  ];
  if (!e2.is_folder) {
    rows.push(['Số mảnh', String(e2.chunk_count)]);
    rows.push(['Cỡ mảnh', dungLuong(e2.chunk_size)]);
    if (e2.sha256) rows.push(['SHA-256', e2.sha256]);
  }
  const than = el('div', {});
  than.appendChild(el('div', { class: 'kv-grid' }, rows.map(([k, v]) =>
    el('div', { class: 'kv' }, el('span', { text: k }),
       el('strong', { text: String(v), style: 'word-break:break-all' })))));

  if (chiTiet && chiTiet.chunks && chiTiet.chunks.length) {
    than.appendChild(el('h4', { text: 'Các mảnh dữ liệu', style: 'margin:14px 0 6px' }));
    const wrap = el('div', { class: 'table-wrap' });
    const table = el('table', { class: 'table' },
      el('thead', {}, el('tr', {},
        el('th', {}, '#'), el('th', {}, 'Vị trí'), el('th', {}, 'Kích thước'),
        el('th', {}, 'DC'), el('th', {}, 'Tài khoản'), el('th', {}, 'Thông điệp'))),
      el('tbody', {}, chiTiet.chunks.map((c) => el('tr', {},
        el('td', {}, String(c.index + 1)),
        el('td', {}, dungLuong(c.offset)),
        el('td', {}, c.size_text),
        el('td', {}, c.dc_id ? 'DC' + c.dc_id : '—'),
        el('td', {}, c.account_id ? '#' + c.account_id : 'nội bộ'),
        el('td', {}, String(c.message_id))))));
    wrap.appendChild(table);
    than.appendChild(wrap);
  }
  HopThoai.mo('Chi tiết', than,
    [el('button', { class: 'btn btn-primary', onclick: HopThoai.dong }, 'Đóng')], true);
}

async function hopThoaiChiaSe(m) {
  const than = el('div', {});
  const trangThai = el('div', { class: 'muted' });
  const oLink = el('input', { class: 'input', readonly: 'readonly', style: 'width:100%' });
  const chonHan = el('select', { class: 'select' },
    el('option', { value: '0' }, 'Không hết hạn'),
    el('option', { value: '3600' }, '1 giờ'),
    el('option', { value: '86400' }, '1 ngày'),
    el('option', { value: '604800' }, '7 ngày'),
    el('option', { value: '2592000' }, '30 ngày'));

  than.appendChild(el('p', { class: 'muted', style: 'margin:0',
    text: 'Ai có liên kết đều tải được tệp này mà không cần đăng nhập.' }));
  than.appendChild(el('div', { class: 'setting' },
    el('label', { text: 'Thời hạn' }), chonHan));
  than.appendChild(el('div', { class: 'setting' },
    el('label', { text: 'Liên kết' }), oLink));
  than.appendChild(trangThai);

  const nutTao = el('button', { class: 'btn btn-primary', onclick: async () => {
    try {
      const kq = await api('/api/files/share', { method: 'POST',
        body: { id: m.id, expires_seconds: Number(chonHan.value) } });
      oLink.value = kq.url;
      oLink.select();
      trangThai.textContent = 'Đã tạo liên kết. Bấm “Sao chép” để lấy.';
      napDanhSach();
    } catch (err) { trangThai.textContent = 'Lỗi: ' + err.message; }
  } }, m.shared ? 'Tạo lại liên kết' : 'Tạo liên kết');

  const nutChep = el('button', { class: 'btn btn-ghost', onclick: async () => {
    if (!oLink.value) return;
    try {
      await navigator.clipboard.writeText(oLink.value);
      thongBao('Đã sao chép liên kết', '', 'ok', 2);
    } catch (e) { oLink.select(); document.execCommand('copy'); }
  } }, 'Sao chép');

  const nutThuHoi = el('button', { class: 'btn btn-ghost danger', onclick: async () => {
    await api('/api/files/unshare', { method: 'POST', body: { id: m.id } });
    oLink.value = '';
    trangThai.textContent = 'Đã thu hồi liên kết.';
    napDanhSach();
  } }, 'Thu hồi');

  if (m.shared && m.share_token) {
    oLink.value = location.origin + '/s/' + m.share_token;
    trangThai.textContent = m.share_expires_at
      ? 'Hết hạn: ' + ngayGio(m.share_expires_at) : 'Liên kết không hết hạn.';
  }
  HopThoai.mo('Chia sẻ “' + m.name + '”', than,
    [nutThuHoi, nutChep, nutTao,
     el('button', { class: 'btn btn-ghost', onclick: HopThoai.dong }, 'Đóng')]);
}

/* ── Thư mục mới ───────────────────────────────────────────────────────── */
$('#nut-thu-muc-moi').addEventListener('click', async () => {
  const ten = await HopThoai.nhap('Thư mục mới', 'Tên thư mục', 'Thư mục mới', 'Tạo');
  if (!ten) return;
  try {
    await api('/api/folders', { method: 'POST', body: { parent: S.duongDan, name: ten } });
    thongBao('Đã tạo thư mục', ten, 'ok', 3);
    napDanhSach();
  } catch (err) { thongBao('Không tạo được thư mục', err.message, 'err'); }
});

/* ══════════════════════════════════════════════════════════════════════════
   Tải lên
   ══════════════════════════════════════════════════════════════════════════ */
$('#nut-tai-len').addEventListener('click', () => $('#chon-tep').click());
$('#chon-tep').addEventListener('change', (e) => {
  const files = Array.from(e.target.files || []);
  e.target.value = '';
  if (files.length) batDauTaiLen(files);
});

let demKeo = 0;
window.addEventListener('dragenter', (e) => {
  if (!e.dataTransfer || !Array.from(e.dataTransfer.types || []).includes('Files')) return;
  demKeo++;
  $('#vung-tha').classList.add('active');
});
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('dragleave', () => {
  if (--demKeo <= 0) { demKeo = 0; $('#vung-tha').classList.remove('active'); }
});
window.addEventListener('drop', (e) => {
  e.preventDefault();
  demKeo = 0;
  $('#vung-tha').classList.remove('active');
  const files = Array.from((e.dataTransfer && e.dataTransfer.files) || []);
  if (files.length) batDauTaiLen(files);
});

async function batDauTaiLen(files) {
  chuyenView('tai-len');
  for (const f of files) await taiMotTep(f);
  napDanhSach();
  capNhatThongKeNhanh();
}

async function taiMotTep(file, chinhSach = 'ask') {
  const theoDoi = {
    id: 'tam-' + Math.random().toString(36).slice(2),
    ten: file.name,
    tong: file.size,
    daGui: 0,
    trangThai: 'Đang chuẩn bị',
    lop: '',
    batDau: Date.now(),
    controller: null,
    huy: false,
  };
  S.phienTaiLen.set(theoDoi.id, theoDoi);
  veDanhSachTaiLen();

  let quickHash = '';
  try {
    theoDoi.trangThai = 'Đang kiểm tra trùng lặp';
    veDanhSachTaiLen();
    quickHash = await bamNhanh(file);
  } catch (e) { /* bỏ qua, vẫn tải lên bình thường */ }

  let init;
  try {
    init = await api('/api/upload/init', {
      method: 'POST',
      body: {
        name: file.name,
        path: S.duongDan,
        size: file.size,
        mime_type: file.type || '',
        quick_hash: quickHash,
        policy: chinhSach,
      },
    });
  } catch (err) {
    theoDoi.trangThai = 'Lỗi: ' + err.message;
    theoDoi.lop = 'err';
    veDanhSachTaiLen();
    thongBao('Không bắt đầu được', err.message, 'err');
    return;
  }

  if (init.needs_decision) {
    S.phienTaiLen.delete(theoDoi.id);
    veDanhSachTaiLen();
    const chon = await hopThoaiTrungLap(file, init.duplicates);
    if (!chon || chon === 'skip') {
      thongBao('Đã bỏ qua', file.name, 'warn', 3);
      return;
    }
    return taiMotTep(file, chon);
  }
  if (init.skipped) {
    theoDoi.trangThai = 'Đã bỏ qua (tệp đã tồn tại)';
    theoDoi.lop = 'cancel';
    veDanhSachTaiLen();
    return;
  }
  if (init.linked) {
    theoDoi.trangThai = 'Đã liên kết tới dữ liệu có sẵn';
    theoDoi.lop = 'done';
    theoDoi.daGui = file.size;
    veDanhSachTaiLen();
    thongBao('Đã liên kết', init.message, 'ok', 4);
    return;
  }

  // Chuyển sang theo dõi bằng mã phiên thật.
  S.phienTaiLen.delete(theoDoi.id);
  theoDoi.id = init.upload_id;
  theoDoi.trangThai = 'Đang tải lên';
  S.phienTaiLen.set(theoDoi.id, theoDoi);
  veDanhSachTaiLen();

  const buoc = Math.max(256 * 1024, Number(init.browser_chunk_size) || 8 * 1024 * 1024);
  let offset = 0;

  try {
    let luotHong = 0;   // số lần hỏng liên tiếp của khối hiện tại
    while (offset < file.size) {
      if (theoDoi.huy) throw new Error('Đã huỷ');
      const den = Math.min(offset + buoc, file.size);
      const lat = file.slice(offset, den);
      theoDoi.controller = new AbortController();

      let res;
      try {
        res = await fetch(`/api/upload/${theoDoi.id}/data`, {
          method: 'PUT',
          credentials: 'same-origin',
          headers: {
            'Content-Type': 'application/octet-stream',
            // Nói rõ mình đang gửi tiếp từ đâu. Máy chủ trả 409 nếu lệch,
            // nên nối lại sai vị trí không thể làm hỏng dữ liệu âm thầm.
            'X-Upload-Offset': String(offset),
          },
          body: lat,
          signal: theoDoi.controller.signal,
        });
      } catch (loiMang) {
        // Người dùng bấm huỷ cũng ném AbortError — phân biệt bằng cờ huỷ.
        if (theoDoi.huy) throw new Error('Đã huỷ');
        res = null;
        theoDoi.loiCuoi = loiMang.message || 'mất kết nối';
      }

      // Rớt mạng, máy chủ khởi động lại, hoặc lỗi 5xx tạm thời → chờ rồi hỏi
      // máy chủ đã nhận tới đâu, cắt lại từ đúng chỗ đó và gửi tiếp. Phiên tải
      // lên phía máy chủ vẫn sống (mặc định 30 phút không hoạt động mới dọn).
      const dangHong = !res || res.status === 409 || res.status >= 500;
      if (dangHong) {
        if (res && res.status !== 409) {
          try { const j = await res.json(); if (j && j.error) theoDoi.loiCuoi = j.error; }
          catch (e) { theoDoi.loiCuoi = `máy chủ trả về ${res.status}`; }
        }
        luotHong++;
        if (luotHong > SO_LAN_THU_LAI) {
          throw new Error(`${theoDoi.loiCuoi || 'mất kết nối'} — đã thử lại ` +
                          `${SO_LAN_THU_LAI} lần`);
        }
        const cho = Math.min(30000, 1000 * Math.pow(2, luotHong - 1));  // 1·2·4·8·16·30 s
        theoDoi.trangThai = `Mất kết nối — thử lại lần ${luotHong}/${SO_LAN_THU_LAI}` +
                            ` sau ${Math.round(cho / 1000)}s`;
        theoDoi.lop = 'warn';
        veDanhSachTaiLen();
        await nguQuaHuy(cho, theoDoi);
        if (theoDoi.huy) throw new Error('Đã huỷ');

        // Hỏi máy chủ đã nhận được bao nhiêu byte rồi cắt lại từ đó.
        const tt = await hoiViTriTaiLen(theoDoi.id);
        if (tt.mat) throw new Error('Phiên tải lên đã hết hạn ở máy chủ');
        if (tt.chuaBiet) continue;   // mạng vẫn hỏng — chờ tiếp ở vòng sau
        offset = tt.viTri;
        theoDoi.daGui = tt.viTri;
        theoDoi.trangThai = 'Đang tải lên (đã nối lại)';
        theoDoi.lop = '';
        veDanhSachTaiLen();
        continue;
      }

      if (!res.ok) {
        // 4xx khác 409: lỗi thật (hết hạn phiên, sai quyền…) — thử lại vô ích.
        let msg = `Máy chủ trả về ${res.status}`;
        try { const j = await res.json(); if (j && j.error) msg = j.error; } catch (e) {}
        throw new Error(msg);
      }
      const kq = await res.json();
      luotHong = 0;
      offset = den;
      theoDoi.daGui = kq.received || offset;
      theoDoi.taiKhoan = kq.account || '';
      theoDoi.mangHienTai = (kq.chunk_index || 0) + 1;
      theoDoi.tongMang = kq.chunk_total || 0;
      veDanhSachTaiLen();
    }

    theoDoi.trangThai = 'Đang hoàn tất';
    veDanhSachTaiLen();
    const xong = await api(`/api/upload/${theoDoi.id}/complete`, { method: 'POST', body: {} });
    theoDoi.trangThai = 'Hoàn tất';
    theoDoi.lop = 'done';
    theoDoi.daGui = theoDoi.tong;
    henGoThe(theoDoi);
    veDanhSachTaiLen();
    thongBao('Đã tải lên', `${file.name} · ${dungLuong(file.size)}`, 'ok', 4);
    if (xong && xong.entry && S.view === 'tep') napDanhSach();
  } catch (err) {
    if (theoDoi.huy) {
      theoDoi.trangThai = 'Đã huỷ';
      theoDoi.lop = 'cancel';
      henGoThe(theoDoi);
    } else {
      // Thẻ lỗi ở lại để người dùng đọc được lý do; tự đóng bằng nút ×.
      theoDoi.trangThai = 'Lỗi: ' + err.message;
      theoDoi.lop = 'err';
      thongBao('Tải lên thất bại', `${file.name}: ${err.message}`, 'err', 8);
      try {
        await api(`/api/upload/${theoDoi.id}/cancel`,
                  { method: 'POST', body: { reason: err.message } });
      } catch (e) { /* phiên có thể đã đóng */ }
    }
    veDanhSachTaiLen();
  }
}

function hopThoaiTrungLap(file, duplicates) {
  return new Promise((resolve) => {
    const than = el('div', {});
    than.appendChild(el('p', { style: 'margin:0 0 4px',
      html: `Tệp <strong>${thoat(file.name)}</strong> (${dungLuong(file.size)}) ` +
            `có thể đã có trên ổ đĩa:` }));
    than.appendChild(el('div', { class: 'dup-list' }, duplicates.map((d) =>
      el('div', { class: 'dup-item' },
        el('span', {}, BIEU_TUONG.other),
        el('div', { class: 'grow', style: 'min-width:0' },
          el('div', { style: 'font-weight:600;overflow:hidden;text-overflow:ellipsis',
                      text: d.path }),
          el('small', { class: 'muted',
            text: `${d.size_text} · sửa lúc ${ngayGio(d.modified_at)}` })),
        el('span', { class: 'reason', text: d.reason })))));

    const luaChon = [
      ['keep_both', 'Giữ cả hai',
       'Tải lên bản mới và tự đổi tên, ví dụ “tệp (2).zip”.'],
      ['link', 'Dùng lại dữ liệu đã có',
       'Không tải lên lại — tạo mục mới trỏ tới cùng dữ liệu. Nhanh nhất, không tốn thêm dung lượng.'],
      ['replace', 'Ghi đè tệp cũ',
       'Xoá dữ liệu cũ trên Telegram và thay bằng bản mới.'],
      ['skip', 'Bỏ qua tệp này', 'Không tải lên, giữ nguyên tệp cũ.'],
    ];
    const box = el('div', { class: 'choice-list' });
    luaChon.forEach(([giaTri, ten, moTa], i) => {
      box.appendChild(el('label', { class: 'choice' },
        el('input', { type: 'radio', name: 'chinh-sach', value: giaTri,
                      checked: i === 0 ? 'checked' : null }),
        el('div', {}, el('strong', { text: ten }), el('small', { text: moTa }))));
    });
    than.appendChild(el('h4', { text: 'Bạn muốn làm gì?', style: 'margin:14px 0 2px' }));
    than.appendChild(box);

    const huy = el('button', { class: 'btn btn-ghost',
      onclick: () => { HopThoai.dong(); resolve(null); } }, 'Huỷ tải lên');
    const ok = el('button', { class: 'btn btn-primary', onclick: () => {
      const chon = box.querySelector('input:checked');
      HopThoai.dong();
      resolve(chon ? chon.value : null);
    } }, 'Tiếp tục');
    HopThoai.mo('Phát hiện tệp trùng', than, [huy, ok]);
  });
}

// Giữ thẻ đã xong lại vài giây cho người dùng kịp thấy rồi tự gỡ. Thẻ lỗi thì
// ở lại tới khi người dùng tự đóng, vì đó là thông tin cần đọc.
const GIU_THE_XONG_MS = 6000;

// Số lần thử lại một khối trước khi chịu thua. Sáu lần với giãn cách 1·2·4·8·16·30
// giây là khoảng một phút — đủ qua một lần rớt Wi-Fi, đổi sóng 4G, hay máy chủ
// khởi động lại, mà không treo mãi khi mạng chết hẳn.
const SO_LAN_THU_LAI = 6;

// Ngủ nhưng tỉnh ngay nếu người dùng bấm huỷ giữa chừng.
function nguQuaHuy(ms, theoDoi) {
  return new Promise((resolve) => {
    const buoc = 200;
    let daCho = 0;
    const dem = setInterval(() => {
      daCho += buoc;
      if (theoDoi.huy || daCho >= ms) { clearInterval(dem); resolve(); }
    }, buoc);
  });
}

// Hỏi máy chủ đã nhận được bao nhiêu byte của phiên này. Phải phân biệt rõ ba
// tình huống: biết được vị trí, phiên đã mất hẳn (thử lại vô ích), và chưa hỏi
// được vì mạng vẫn hỏng (phải thử lại tiếp).
async function hoiViTriTaiLen(id) {
  try {
    const res = await fetch(`/api/upload/${id}`, { credentials: 'same-origin' });
    if (res.status === 404) return { mat: true };
    if (!res.ok) return { chuaBiet: true };
    const j = await res.json();
    return { viTri: Number(j.received) || 0 };
  } catch (e) {
    return { chuaBiet: true };   // vẫn chưa có mạng
  }
}

function henGoThe(theoDoi, cho = GIU_THE_XONG_MS) {
  if (theoDoi.hengio) clearTimeout(theoDoi.hengio);
  theoDoi.hengio = setTimeout(() => {
    S.phienTaiLen.delete(theoDoi.id);
    veDanhSachTaiLen();
  }, cho);
}

function veDanhSachTaiLen() {
  const box = $('#danh-sach-tai-len');
  const list = Array.from(S.phienTaiLen.values());
  $('#tai-len-trong').hidden = list.length > 0;
  box.innerHTML = '';

  // Thẻ đang đếm ngược để thử lại ('warn') vẫn là phiên đang chạy: phải tính
  // vào huy hiệu và phải cho bấm Huỷ, chứ không phải nút × dọn thẻ.
  const dangHoatDong = (t) => !t.lop || t.lop === 'warn';

  let dangChay = 0;
  for (const t of list) {
    if (dangHoatDong(t)) dangChay++;
    const pct = t.tong ? Math.min(100, (t.daGui / t.tong) * 100) : 0;
    const giay = (Date.now() - t.batDau) / 1000;
    const toc = giay > 0.4 ? t.daGui / giay : 0;
    const conLai = toc > 1 && t.tong > t.daGui ? (t.tong - t.daGui) / toc : 0;

    box.appendChild(el('div', { class: 'upload-item' },
      el('div', { class: 'upload-top' },
        el('span', {}, BIEU_TUONG.other),
        el('span', { class: 'upload-name', text: t.ten, title: t.ten }),
        el('span', { class: 'upload-state ' + (t.lop || ''), text: t.trangThai }),
        dangHoatDong(t)
          ? el('button', { class: 'btn btn-ghost danger', onclick: () => huyTaiLen(t.id) }, 'Huỷ')
          : el('button', {
              class: 'btn btn-ghost', title: 'Bỏ khỏi danh sách',
              onclick: () => {
                if (t.hengio) clearTimeout(t.hengio);
                S.phienTaiLen.delete(t.id);
                veDanhSachTaiLen();
              },
            }, '×')),
      el('div', { class: 'progress ' + (t.lop || '') },
        el('i', { style: `width:${pct}%` })),
      el('div', { class: 'upload-meta' },
        el('span', {}, `${dungLuong(t.daGui)} / ${dungLuong(t.tong)} (${pct.toFixed(1)}%)`),
        el('span', {}, t.tongMang ? `Mảnh ${t.mangHienTai}/${t.tongMang}` : ''),
        el('span', {}, t.taiKhoan ? `Qua: ${t.taiKhoan}` : ''),
        el('span', {}, toc ? tocDo(toc) : ''),
        el('span', {}, conLai ? 'Còn ' + thoiLuong(conLai) : ''))));
  }
  const badge = $('#badge-tai-len');
  badge.hidden = dangChay === 0;
  badge.textContent = String(dangChay);

  // Không còn phiên nào chạy thì nút chỉ dọn danh sách, nhãn phải nói đúng vậy.
  const nut = $('#nut-huy-tat-ca');
  nut.textContent = dangChay ? 'Huỷ tất cả' : 'Xoá danh sách';
  nut.hidden = list.length === 0;
}

async function huyTaiLen(id) {
  const t = S.phienTaiLen.get(id);
  if (!t) return;
  t.huy = true;
  t.trangThai = 'Đang huỷ…';
  veDanhSachTaiLen();
  if (t.controller) { try { t.controller.abort(); } catch (e) {} }
  try {
    await api(`/api/upload/${id}/cancel`, { method: 'POST', body: { reason: 'Người dùng huỷ' } });
    t.trangThai = 'Đã huỷ';
    t.lop = 'cancel';
    thongBao('Đã huỷ tải lên', t.ten + ' — dữ liệu đã đẩy lên được dọn sạch.', 'warn', 5);
  } catch (err) {
    t.trangThai = 'Đã huỷ';
    t.lop = 'cancel';
  }
  henGoThe(t);
  veDanhSachTaiLen();
}

$('#nut-huy-tat-ca').addEventListener('click', async () => {
  const dangChay = Array.from(S.phienTaiLen.values()).filter((t) => !t.lop);
  if (!dangChay.length) {
    for (const t of S.phienTaiLen.values()) if (t.hengio) clearTimeout(t.hengio);
    S.phienTaiLen.clear();
    veDanhSachTaiLen();
    return;
  }
  const ok = await HopThoai.xacNhan('Huỷ tất cả',
    `Huỷ ${dangChay.length} phiên tải lên đang chạy? Dữ liệu đã đẩy lên sẽ bị xoá.`,
    'Huỷ tất cả', true);
  if (!ok) return;
  for (const t of dangChay) await huyTaiLen(t.id);
});

// Đồng bộ với máy chủ (bắt cả các phiên do WebDAV hoặc tab khác tạo).
async function dongBoTaiLen() {
  if (!S.nguoiDung) return;
  try {
    const kq = await api('/api/uploads');
    const conSong = new Set();
    for (const u of kq.uploads || []) {
      conSong.add(u.id);
      const cu = S.phienTaiLen.get(u.id);
      if (cu) {
        // Phiên của chính tab này do vòng tải lên tự cập nhật — không đụng vào,
        // tránh giẫm lên nhau.
        if (!cu.ngoai) continue;
        cu.ten = u.name;
        cu.tong = u.total;
        cu.daGui = u.received;
        cu.trangThai = u.state_text;
        cu.taiKhoan = u.account;
        cu.mangHienTai = u.chunk_index + 1;
        cu.tongMang = u.chunk_total;
        continue;
      }
      S.phienTaiLen.set(u.id, {
        id: u.id, ten: u.name, tong: u.total, daGui: u.received,
        trangThai: u.state_text, lop: '', batDau: Date.now() - 1000,
        taiKhoan: u.account, mangHienTai: u.chunk_index + 1, tongMang: u.chunk_total,
        ngoai: true,
      });
    }
    // Phiên bên ngoài đã biến mất khỏi máy chủ nghĩa là nó xong (hoặc bị huỷ).
    // Không dọn thì thẻ đứng im mãi và huy hiệu sáng hoài.
    for (const t of Array.from(S.phienTaiLen.values())) {
      if (!t.ngoai || t.lop || conSong.has(t.id)) continue;
      t.lop = 'done';
      t.trangThai = 'Hoàn tất';
      t.daGui = t.tong;
      henGoThe(t);
    }
    if (S.view === 'tai-len') veDanhSachTaiLen();
    // Thẻ 'warn' đang đếm ngược thử lại vẫn tính là đang chạy — giống hệt
    // cách veDanhSachTaiLen() đếm, để huy hiệu không nhấp nháy lệch nhau.
    const dangChay = Array.from(S.phienTaiLen.values())
                          .filter((t) => !t.lop || t.lop === 'warn').length;
    const badge = $('#badge-tai-len');
    badge.hidden = dangChay === 0;
    badge.textContent = String(dangChay);
  } catch (e) { /* im lặng */ }
}

// Cảnh báo khi đóng tab lúc đang tải.
window.addEventListener('beforeunload', (e) => {
  const dangChay = Array.from(S.phienTaiLen.values()).filter((t) => !t.lop && !t.ngoai);
  if (!dangChay.length) return;
  for (const t of dangChay) {
    try {
      navigator.sendBeacon(`/api/upload/${t.id}/cancel`,
        new Blob([JSON.stringify({ reason: 'Đóng trình duyệt' })],
                 { type: 'application/json' }));
    } catch (err) { /* bỏ qua */ }
  }
  e.preventDefault();
  e.returnValue = '';
});

/* ══════════════════════════════════════════════════════════════════════════
   Thùng rác
   ══════════════════════════════════════════════════════════════════════════ */
async function napThungRac() {
  try {
    const kq = await api('/api/files?trash=1&limit=500');
    const box = $('#danh-sach-thung-rac');
    box.innerHTML = '';
    const list = kq.entries || [];
    $('#thung-rac-trong').hidden = list.length > 0;
    $('#thung-rac-mo-ta').textContent = list.length ? `${list.length} mục` : '';

    for (const m of list) {
      const loai = loaiCuaMuc(m);
      box.appendChild(el('div', { class: 'row-item' },
        el('div', { class: 'row-icon ft-' + loai }, BIEU_TUONG[loai] || BIEU_TUONG.other),
        el('div', { class: 'row-name' },
          el('span', { text: m.name }),
          el('small', { class: 'muted', text: ' · ' + m.path })),
        el('div', { class: 'row-size', text: m.is_folder ? '—' : dungLuong(m.size) }),
        el('div', { class: 'row-date', text: 'Xoá lúc ' + ngayGio(m.trashed_at) }),
        el('div', { style: 'display:flex;gap:6px' },
          el('button', { class: 'btn btn-ghost', onclick: async () => {
            await api('/api/files/restore', { method: 'POST', body: { ids: [m.id] } });
            thongBao('Đã khôi phục', m.name, 'ok', 3);
            napThungRac(); capNhatThongKeNhanh();
          } }, 'Khôi phục'),
          el('button', { class: 'btn btn-ghost danger', onclick: async () => {
            const ok = await HopThoai.xacNhan('Xoá vĩnh viễn',
              `Xoá vĩnh viễn “${m.name}”? Dữ liệu trên Telegram cũng bị xoá và ` +
              `không thể khôi phục.`, 'Xoá vĩnh viễn', true);
            if (!ok) return;
            await api('/api/files/delete', { method: 'POST', body: { ids: [m.id] } });
            thongBao('Đã xoá vĩnh viễn', m.name, 'ok', 3);
            napThungRac(); capNhatThongKeNhanh();
          } }, 'Xoá hẳn'))));
    }
  } catch (err) { thongBao('Không tải được thùng rác', err.message, 'err'); }
}

$('#nut-don-thung-rac').addEventListener('click', async () => {
  const ok = await HopThoai.xacNhan('Dọn sạch thùng rác',
    'Xoá vĩnh viễn toàn bộ mục trong thùng rác? Dữ liệu trên Telegram cũng bị xoá.',
    'Dọn sạch', true);
  if (!ok) return;
  try {
    const kq = await api('/api/trash/empty', { method: 'POST' });
    thongBao('Đã dọn thùng rác', kq.message, 'ok', 4);
    napThungRac(); capNhatThongKeNhanh();
  } catch (err) { thongBao('Không dọn được', err.message, 'err'); }
});

/* ══════════════════════════════════════════════════════════════════════════
   Thống kê
   ══════════════════════════════════════════════════════════════════════════ */
async function capNhatThongKeNhanh() {
  if (!S.nguoiDung) return;
  try {
    const s = await api('/api/stats');
    S.thongKe = s;
    $('#dl-tong').textContent = s.storage.total_text;
    $('#dl-tong').title = `Chiếm thật trên Telegram: ${s.storage.physical_text}` +
      (s.storage.saved_bytes ? ` (tiết kiệm ${s.storage.saved_text} nhờ khử trùng lặp)` : '');
    $('#dl-so-tep').textContent = `${s.storage.file_count} tệp · ${s.storage.folder_count} thư mục`;
    $('#dl-nguon').textContent = s.backend.name +
      (s.backend.total_accounts ? ` · ${s.backend.ready_accounts}/${s.backend.total_accounts} tk`
                                : '');
    const gioiHan = (s.quota && s.quota.limit) ? s.quota.limit : 0;
    const pct = gioiHan ? Math.min(100, (s.quota.used / gioiHan) * 100)
                        : Math.min(100, Math.log10(1 + s.storage.total_bytes / MB) * 12);
    $('#dl-thanh').style.width = pct.toFixed(1) + '%';
  } catch (e) { /* im lặng */ }
}

function theThongKe(nhan, giaTri, phu, mau, icon) {
  return el('div', { class: 'stat-card' },
    el('div', { class: 'top' },
      el('span', { class: 'label', text: nhan }),
      el('span', { class: 'stat-icon ft-' + mau }, icon)),
    el('div', { class: 'value', text: giaTri }),
    el('div', { class: 'sub', text: phu || '' }));
}

async function napThongKe() {
  try {
    const s = await api('/api/stats');
    S.thongKe = s;
    const g = $('#luoi-thong-ke');
    g.innerHTML = '';
    g.appendChild(theThongKe('Tổng dung lượng đã dùng', s.storage.total_text,
      `${s.storage.file_count} tệp trong ${s.storage.folder_count} thư mục`, 'image', '💾'));
    g.appendChild(theThongKe('Chiếm thật trên Telegram', s.storage.physical_text,
      s.storage.saved_bytes
        ? `Tiết kiệm ${s.storage.saved_text} nhờ khử trùng lặp`
        : 'Không có tệp nào trùng nội dung', 'video', '☁️'));
    g.appendChild(theThongKe('Số mảnh trên Telegram', String(s.storage.unique_chunk_count),
      `${s.storage.chunk_count} lượt tham chiếu · cỡ mảnh ${s.storage.chunk_size_text}`,
      'video', '🧩'));
    g.appendChild(theThongKe('Đã đẩy lên', s.storage.uploaded_text,
      'Tính từ lúc khởi động', 'code', '⬆️'));
    g.appendChild(theThongKe('Đã tải về', s.storage.downloaded_text,
      `Bộ đệm: ${s.cache.used_text} / ${s.cache.capacity_text}`, 'spreadsheet', '⬇️'));
    g.appendChild(theThongKe('Thùng rác', s.storage.trashed_text,
      `${s.storage.trashed_count} mục chờ dọn`, 'archive', '🗑️'));
    g.appendChild(theThongKe('Tài khoản Telegram',
      `${s.backend.ready_accounts}/${s.backend.total_accounts}`,
      s.backend.ready ? 'Sẵn sàng' : (s.backend.message || 'Chưa sẵn sàng'),
      s.backend.ready ? 'code' : 'pdf', s.backend.ready ? '✅' : '⚠️'));
    g.appendChild(theThongKe('Thời gian chạy', s.server.uptime_text,
      `${s.server.requests} lượt yêu cầu`, 'document', '⏱️'));
    g.appendChild(theThongKe('Đĩa trống trên máy chủ', s.system.free_disk_text,
      `RAM khả dụng: ${s.system.available_memory_text}`, 'text', '🖥️'));

    const kv = $('#chi-tiet-he-thong');
    kv.innerHTML = '';
    const rows = [
      ['Nơi lưu trữ', s.backend.name],
      ['Siêu nhóm', s.backend.channel_title || '(chưa chọn)'],
      ['Cơ sở dữ liệu', s.system.database],
      ['Chế độ đệm khi tải lên', s.storage.buffer_mode],
      ['Kích thước mảnh', s.storage.chunk_size_text],
      ['Múi giờ', s.system.timezone],
      ['Giờ máy chủ', s.system.server_time],
      ['Tổng RAM máy chủ', s.system.total_memory_text],
      ['Bộ đệm tải xuống', `${s.cache.used_text} / ${s.cache.capacity_text} ` +
        `(trúng ${s.cache.hits}, trượt ${s.cache.misses})`],
      ['Lưu lượng đã gửi', s.server.bytes_sent_text],
      ['Lưu lượng đã nhận', s.server.bytes_received_text],
      ['Kết nối đang mở', String(s.server.connections)],
      ['Nhật ký', `${s.logs.error} lỗi · ${s.logs.warn} cảnh báo · ${s.logs.info} thông tin`],
    ];
    if (S.phienBan) {
      rows.push(['Phiên bản', `${S.phienBan.version} (build ${S.phienBan.build})`]);
      rows.push(['Bản dựng', `${S.phienBan.commit} · ${S.phienBan.build_time}`]);
    }
    for (const [k, v] of rows) {
      kv.appendChild(el('div', { class: 'kv' },
        el('span', { text: k }), el('strong', { text: String(v) })));
    }
  } catch (err) { thongBao('Không tải được thống kê', err.message, 'err'); }
}

/* ══════════════════════════════════════════════════════════════════════════
   Nhật ký
   ══════════════════════════════════════════════════════════════════════════ */
function themDongNhatKy(r) {
  const khung = $('#khung-nhat-ky');
  const cuoi = khung.scrollTop + khung.clientHeight >= khung.scrollHeight - 40;
  khung.appendChild(el('div', { class: 'log-line' },
    el('span', { class: 'log-time', text: r.time }),
    el('span', { class: 'log-lvl lvl-' + r.level, text: r.level }),
    el('span', { class: 'log-tag', text: r.tag }),
    el('span', { class: 'log-msg', text: r.message })));
  while (khung.childNodes.length > 3000) khung.removeChild(khung.firstChild);
  if ($('#tu-cuon').checked && cuoi) khung.scrollTop = khung.scrollHeight;
}

async function batNhatKy() {
  tatNhatKy();
  const khung = $('#khung-nhat-ky');
  khung.innerHTML = '';
  const muc = $('#loc-muc').value;
  const tuKhoa = $('#loc-tu-khoa').value.trim();

  let lastSeq = 0;
  try {
    const kq = await api(`/api/logs?limit=400&level=${muc}&filter=${encodeURIComponent(tuKhoa)}`);
    for (const r of kq.records || []) { themDongNhatKy(r); lastSeq = r.seq; }
    if (!kq.records || !kq.records.length) lastSeq = kq.last_seq || 0;
    khung.scrollTop = khung.scrollHeight;
  } catch (e) { /* tiếp tục với luồng trực tiếp */ }

  const url = `/api/logs/stream?after=${lastSeq}&level=${muc}` +
              `&filter=${encodeURIComponent(tuKhoa)}`;
  try {
    S.nhatKySse = new EventSource(url, { withCredentials: true });
    S.nhatKySse.onmessage = (ev) => {
      try { themDongNhatKy(JSON.parse(ev.data)); } catch (e) {}
    };
    S.nhatKySse.onerror = () => { /* trình duyệt tự kết nối lại */ };
  } catch (e) { /* trình duyệt cũ: bỏ qua luồng trực tiếp */ }
}
function tatNhatKy() {
  if (S.nhatKySse) { S.nhatKySse.close(); S.nhatKySse = null; }
}
$('#loc-muc').addEventListener('change', () => { if (S.view === 'nhat-ky') batNhatKy(); });
let timerLoc = null;
$('#loc-tu-khoa').addEventListener('input', () => {
  clearTimeout(timerLoc);
  timerLoc = setTimeout(() => { if (S.view === 'nhat-ky') batNhatKy(); }, 420);
});
$('#nut-xoa-nhat-ky').addEventListener('click', () => { $('#khung-nhat-ky').innerHTML = ''; });

/* ══════════════════════════════════════════════════════════════════════════
   Tài khoản Telegram
   ══════════════════════════════════════════════════════════════════════════ */
async function napTaiKhoan() {
  try {
    const kq = await api('/api/accounts');
    const box = $('#danh-sach-tai-khoan');
    box.innerHTML = '';
    if (kq.backend === 'local') {
      box.appendChild(el('div', { class: 'empty small' },
        el('p', {}, 'Ứng dụng đang chạy ở chế độ thử nghiệm (lưu nội bộ). ' +
                    'Hãy đổi “Nơi lưu trữ” sang Telegram trong phần Cài đặt.')));
      return;
    }
    if (!kq.accounts.length) {
      box.appendChild(el('div', { class: 'empty small' },
        el('p', {}, 'Chưa có tài khoản nào. Bấm “Thêm tài khoản” để bắt đầu.')));
    }
    for (const a of kq.accounts) {
      const mau = !a.enabled ? 'off' : a.authorized && a.connected ? 'ok'
                : a.authorized ? 'warn' : 'err';
      box.appendChild(el('div', { class: 'account-card' },
        el('div', { class: 'account-head' },
          el('div', { class: 'account-avatar',
                      text: (a.display_name || a.label || '?').trim().charAt(0).toUpperCase() }),
          el('div', { class: 'account-title' },
            el('strong', { text: a.display_name || a.label }),
            el('small', { text: a.phone ? '+' + a.phone : ('#' + a.id) })),
          el('span', { class: 'dot ' + mau, title: a.status })),
        el('div', { class: 'account-stats' },
          el('span', {}, a.status),
          el('span', {}, 'DC' + a.home_dc),
          el('span', {}, '⬆ ' + a.uploaded_text),
          el('span', {}, '⬇ ' + a.downloaded_text),
          a.active_uploads ? el('span', {}, `${a.active_uploads} việc`) : null),
        a.last_error ? el('div', { class: 'account-error', text: a.last_error }) : null,
        el('div', { class: 'account-actions' },
          el('button', { class: 'btn btn-ghost', onclick: async () => {
            await api('/api/accounts/toggle',
                      { method: 'POST', body: { account_id: a.id, enabled: !a.enabled } });
            napTaiKhoan();
          } }, a.enabled ? 'Tắt' : 'Bật'),
          el('button', { class: 'btn btn-ghost danger', onclick: async () => {
            const ok = await HopThoai.xacNhan('Gỡ tài khoản',
              `Gỡ tài khoản “${a.display_name || a.label}”? Ứng dụng sẽ đăng xuất khỏi ` +
              `Telegram. Các tệp đã lưu vẫn còn nếu tài khoản khác truy cập được nhóm.`,
              'Gỡ tài khoản', true);
            if (!ok) return;
            await api('/api/accounts/remove', { method: 'POST', body: { account_id: a.id } });
            thongBao('Đã gỡ tài khoản', '', 'ok', 3);
            napTaiKhoan();
          } }, 'Gỡ'))));
    }
  } catch (err) { thongBao('Không tải được tài khoản', err.message, 'err'); }

  try {
    const st = await api('/api/stats');
    $('#nhom-hien-tai').innerHTML = st.backend.channel_title
      ? `Đang lưu vào: <strong>${thoat(st.backend.channel_title)}</strong> ` +
        `<span class="muted">(id ${st.backend.channel_id})</span>`
      : '<span class="muted">Chưa chọn siêu nhóm lưu trữ.</span>';
  } catch (e) {}
}

$('#nut-ket-noi-lai').addEventListener('click', async () => {
  try {
    await api('/api/accounts/connect', { method: 'POST' });
    thongBao('Đang kết nối lại', 'Xem tiến trình ở tab Nhật ký.', 'info', 4);
    setTimeout(napTaiKhoan, 2500);
  } catch (err) { thongBao('Không kết nối được', err.message, 'err'); }
});

$('#nut-them-tai-khoan').addEventListener('click', () => hopThoaiThemTaiKhoan());

function hopThoaiThemTaiKhoan() {
  const oNhan = el('input', { class: 'input', placeholder: 'Ví dụ: Tài khoản chính' });
  const oSdt = el('input', { class: 'input', placeholder: '+84912345678' });
  const trangThai = el('div', { class: 'muted' });

  const than = el('div', {},
    el('p', { class: 'muted', style: 'margin:0',
      html: 'Cần <strong>api_id</strong> và <strong>api_hash</strong> đã điền trong Cài đặt. ' +
            'Tạo chúng tại <em>my.telegram.org → API development tools</em>.' }),
    el('div', { class: 'setting' }, el('label', { text: 'Tên gợi nhớ' }), oNhan),
    el('div', { class: 'setting' }, el('label', { text: 'Số điện thoại (kèm mã quốc gia)' }), oSdt),
    trangThai);

  const nutGui = el('button', { class: 'btn btn-primary', onclick: async () => {
    const phone = oSdt.value.trim();
    if (!phone) { trangThai.textContent = 'Hãy nhập số điện thoại.'; return; }
    nutGui.disabled = true;
    trangThai.textContent = 'Đang gửi mã xác thực…';
    try {
      const kq = await api('/api/accounts/add', {
        method: 'POST', body: { label: oNhan.value.trim(), phone },
      });
      HopThoai.dong();
      hopThoaiNhapMa(kq.account_id, kq.message, kq.code_length);
    } catch (err) {
      trangThai.textContent = 'Lỗi: ' + err.message;
      nutGui.disabled = false;
    }
  } }, 'Gửi mã xác thực');

  HopThoai.mo('Thêm tài khoản Telegram', than,
    [el('button', { class: 'btn btn-ghost', onclick: HopThoai.dong }, 'Huỷ'), nutGui]);
}

function hopThoaiNhapMa(accountId, thongDiep, doDai) {
  const oMa = el('input', { class: 'input', inputmode: 'numeric',
    placeholder: '_'.repeat(doDai || 5), maxlength: '8',
    style: 'letter-spacing:6px;font-size:20px;text-align:center' });
  const trangThai = el('div', { class: 'muted', text: thongDiep || '' });
  const than = el('div', {},
    el('p', { style: 'margin:0' }, 'Nhập mã Telegram vừa gửi cho bạn.'),
    el('div', { class: 'setting' }, el('label', { text: 'Mã xác thực' }), oMa),
    trangThai);

  const nut = el('button', { class: 'btn btn-primary', onclick: async () => {
    nut.disabled = true;
    trangThai.textContent = 'Đang xác thực…';
    try {
      const kq = await api('/api/accounts/code',
        { method: 'POST', body: { account_id: accountId, code: oMa.value.trim() } });
      if (kq.needs_password) {
        HopThoai.dong();
        hopThoaiNhapMatKhau2Lop(accountId, kq.password_hint);
        return;
      }
      HopThoai.dong();
      thongBao('Đã thêm tài khoản', kq.display_name || '', 'ok', 5);
      napTaiKhoan();
    } catch (err) {
      trangThai.textContent = 'Lỗi: ' + err.message;
      nut.disabled = false;
    }
  } }, 'Xác nhận');

  oMa.addEventListener('keydown', (e) => { if (e.key === 'Enter') nut.click(); });
  HopThoai.mo('Nhập mã xác thực', than,
    [el('button', { class: 'btn btn-ghost', onclick: HopThoai.dong }, 'Để sau'), nut]);
}

function hopThoaiNhapMatKhau2Lop(accountId, goiY) {
  const oMk = el('input', { class: 'input', type: 'password',
                            placeholder: 'Mật khẩu hai lớp' });
  const trangThai = el('div', { class: 'muted',
    text: goiY ? 'Gợi ý từ Telegram: ' + goiY : '' });
  const than = el('div', {},
    el('p', { style: 'margin:0' },
      'Tài khoản này bật xác thực hai lớp. Nhập mật khẩu đám mây của Telegram.'),
    el('div', { class: 'setting' }, el('label', { text: 'Mật khẩu' }), oMk),
    trangThai);

  const nut = el('button', { class: 'btn btn-primary', onclick: async () => {
    nut.disabled = true;
    trangThai.textContent = 'Đang kiểm tra…';
    try {
      const kq = await api('/api/accounts/password',
        { method: 'POST', body: { account_id: accountId, password: oMk.value } });
      HopThoai.dong();
      thongBao('Đã thêm tài khoản', kq.display_name || '', 'ok', 5);
      napTaiKhoan();
    } catch (err) {
      trangThai.textContent = 'Lỗi: ' + err.message;
      nut.disabled = false;
    }
  } }, 'Đăng nhập');

  oMk.addEventListener('keydown', (e) => { if (e.key === 'Enter') nut.click(); });
  HopThoai.mo('Xác thực hai lớp', than,
    [el('button', { class: 'btn btn-ghost', onclick: HopThoai.dong }, 'Huỷ'), nut]);
}

$('#nut-liet-ke-nhom').addEventListener('click', async () => {
  const box = $('#ket-qua-nhom');
  box.innerHTML = '<div class="muted">Đang tải danh sách nhóm…</div>';
  try {
    const kq = await api('/api/telegram/groups');
    box.innerHTML = '';
    if (!kq.groups.length) {
      box.innerHTML = '<div class="muted">Không tìm thấy siêu nhóm nào. ' +
        'Hãy tạo một siêu nhóm và thêm tài khoản vào đó.</div>';
      return;
    }
    for (const g of kq.groups) {
      box.appendChild(el('div', { class: 'group-item' },
        el('div', {}, el('strong', { text: g.title }),
           el('div', { class: 'muted', style: 'font-size:12px', text: 'ID ' + g.id })),
        el('button', { class: 'btn btn-primary', onclick: () => {
          $('#o-sieu-nhom').value = String(g.id);
          $('#nut-luu-sieu-nhom').click();
        } }, 'Chọn')));
    }
  } catch (err) { box.innerHTML = `<div class="muted">Lỗi: ${thoat(err.message)}</div>`; }
});

$('#nut-luu-sieu-nhom').addEventListener('click', async () => {
  const v = $('#o-sieu-nhom').value.trim();
  if (!v) { thongBao('Chưa nhập siêu nhóm', '', 'warn', 3); return; }
  try {
    const kq = await api('/api/telegram/group', { method: 'POST', body: { group: v } });
    thongBao('Đã chọn siêu nhóm', kq.message, 'ok', 5);
    napTaiKhoan();
  } catch (err) { thongBao('Không chọn được nhóm', err.message, 'err', 8); }
});

/* ══════════════════════════════════════════════════════════════════════════
   Người dùng
   ══════════════════════════════════════════════════════════════════════════ */
async function napNguoiDung() {
  try {
    const kq = await api('/api/users');
    const tbody = $('#bang-nguoi-dung tbody');
    tbody.innerHTML = '';
    for (const u of kq.users) {
      tbody.appendChild(el('tr', {},
        el('td', {}, el('strong', { text: u.username })),
        el('td', {}, u.display_name || '—'),
        el('td', {}, u.is_admin ? 'Quản trị viên' : 'Người dùng'),
        el('td', {}, u.usage_text),
        el('td', {}, u.quota_text),
        el('td', {}, u.last_login_text),
        el('td', { class: 'actions' },
          el('button', { class: 'btn btn-ghost', onclick: () => hopThoaiSuaNguoiDung(u) },
             'Sửa'),
          u.id !== S.nguoiDung.id
            ? el('button', { class: 'btn btn-ghost danger', onclick: async () => {
                const ok = await HopThoai.xacNhan('Xoá người dùng',
                  `Xoá tài khoản “${u.username}”? Tệp của họ vẫn giữ nguyên.`, 'Xoá', true);
                if (!ok) return;
                try {
                  await api('/api/users/delete', { method: 'POST', body: { id: u.id } });
                  thongBao('Đã xoá người dùng', u.username, 'ok', 3);
                  napNguoiDung();
                } catch (err) { thongBao('Không xoá được', err.message, 'err'); }
              } }, 'Xoá')
            : null)));
    }
  } catch (err) { thongBao('Không tải được người dùng', err.message, 'err'); }
}

$('#nut-them-nguoi-dung').addEventListener('click', () => {
  const oTen = el('input', { class: 'input' });
  const oHienThi = el('input', { class: 'input' });
  const oMk = el('input', { class: 'input', type: 'password' });
  const oHanMuc = el('input', { class: 'input', placeholder: 'Ví dụ: 100GB (để trống = không giới hạn)' });
  const oQuanTri = el('input', { type: 'checkbox' });
  const trangThai = el('div', { class: 'muted' });

  const than = el('div', {},
    el('div', { class: 'setting' }, el('label', { text: 'Tên đăng nhập' }), oTen),
    el('div', { class: 'setting' }, el('label', { text: 'Tên hiển thị' }), oHienThi),
    el('div', { class: 'setting' }, el('label', { text: 'Mật khẩu' }), oMk),
    el('div', { class: 'setting' }, el('label', { text: 'Hạn mức dung lượng' }), oHanMuc),
    el('label', { class: 'switch' }, oQuanTri, el('span', {}, 'Là quản trị viên')),
    trangThai);

  const nut = el('button', { class: 'btn btn-primary', onclick: async () => {
    try {
      await api('/api/users', { method: 'POST', body: {
        username: oTen.value.trim(), password: oMk.value,
        display_name: oHienThi.value.trim(), is_admin: oQuanTri.checked,
        quota_bytes: oHanMuc.value.trim(),
      } });
      HopThoai.dong();
      thongBao('Đã tạo người dùng', oTen.value, 'ok', 3);
      napNguoiDung();
    } catch (err) { trangThai.textContent = 'Lỗi: ' + err.message; }
  } }, 'Tạo');

  HopThoai.mo('Thêm người dùng', than,
    [el('button', { class: 'btn btn-ghost', onclick: HopThoai.dong }, 'Huỷ'), nut]);
});

function hopThoaiSuaNguoiDung(u) {
  const oHienThi = el('input', { class: 'input', value: u.display_name || '' });
  const oMk = el('input', { class: 'input', type: 'password',
                            placeholder: 'Để trống nếu không đổi' });
  const oHanMuc = el('input', { class: 'input',
    value: u.quota_bytes ? String(u.quota_bytes) : '',
    placeholder: 'Byte hoặc “100GB”, trống = không giới hạn' });
  const oQuanTri = el('input', { type: 'checkbox', checked: u.is_admin ? 'checked' : null });
  const oBat = el('input', { type: 'checkbox', checked: u.enabled ? 'checked' : null });
  const trangThai = el('div', { class: 'muted' });

  const than = el('div', {},
    el('div', { class: 'setting' }, el('label', { text: 'Tên hiển thị' }), oHienThi),
    el('div', { class: 'setting' }, el('label', { text: 'Mật khẩu mới' }), oMk),
    el('div', { class: 'setting' }, el('label', { text: 'Hạn mức dung lượng' }), oHanMuc),
    el('label', { class: 'switch' }, oQuanTri, el('span', {}, 'Là quản trị viên')),
    el('label', { class: 'switch' }, oBat, el('span', {}, 'Cho phép đăng nhập')),
    trangThai);

  const nut = el('button', { class: 'btn btn-primary', onclick: async () => {
    const body = { id: u.id, display_name: oHienThi.value.trim(),
                   is_admin: oQuanTri.checked, enabled: oBat.checked,
                   quota_bytes: oHanMuc.value.trim() };
    if (oMk.value) body.password = oMk.value;
    try {
      await api('/api/users/update', { method: 'POST', body });
      HopThoai.dong();
      thongBao('Đã cập nhật', u.username, 'ok', 3);
      napNguoiDung();
    } catch (err) { trangThai.textContent = 'Lỗi: ' + err.message; }
  } }, 'Lưu');

  HopThoai.mo('Sửa “' + u.username + '”', than,
    [el('button', { class: 'btn btn-ghost', onclick: HopThoai.dong }, 'Huỷ'), nut]);
}

/* ══════════════════════════════════════════════════════════════════════════
   Cài đặt
   ══════════════════════════════════════════════════════════════════════════ */
const MO_TA_CAI_DAT = {
  'storage.chunk_size':
    'Mỗi tệp được cắt thành các mảnh cỡ này rồi đẩy lên Telegram. Nhập kèm đơn vị, ví dụ ' +
    '“500MB”. Telegram cho phép tối đa khoảng 1900 MB mỗi mảnh với tài khoản thường.',
  'storage.buffer_mode':
    'stream = đẩy thẳng lên Telegram, tốn rất ít RAM (khuyên dùng). memory = giữ trọn mảnh ' +
    'trong RAM rồi mới đẩy. disk = ghi ra tệp tạm, hợp với máy ít RAM.',
  'storage.browser_chunk_size':
    'Mỗi lần trình duyệt gửi lên máy chủ bao nhiêu byte. Nhỏ thì tiến độ mượt hơn và huỷ ' +
    'nhanh hơn; lớn thì đỡ tốn vòng lặp mạng.',
  'storage.parallel_chunks':
    'Số mảnh được xử lý song song (mỗi mảnh giao cho một tài khoản Telegram khác nhau).',
  'storage.memory_budget': 'Giới hạn RAM tối đa cho toàn bộ vùng đệm tải lên.',
  'storage.download_cache_bytes':
    'Bộ nhớ đệm khối 1 MB khi tải xuống, giúp tua video mượt mà không tải lại.',
  'storage.deduplicate':
    'Nếu tệp mới trùng nội dung với tệp đã có, dùng lại dữ liệu cũ thay vì tốn thêm dung lượng.',
  'storage.trash_retention_days': 'Số ngày giữ tệp trong thùng rác trước khi tự xoá hẳn (0 = giữ mãi).',
  'storage.upload_idle_timeout_seconds':
    'Phiên tải lên không có hoạt động quá số giây này sẽ tự huỷ và dọn dữ liệu.',
  'telegram.api_id': 'Lấy tại my.telegram.org → API development tools.',
  'telegram.api_hash': 'Đi kèm api_id ở trên.',
  'telegram.backend':
    'telegram = lưu thật lên Telegram. local = lưu trên đĩa máy này để chạy thử.',
  'telegram.obfuscated':
    'Bật lớp nguỵ trang cho kết nối MTProto, giúp vượt qua mạng chặn theo đặc trưng gói tin.',
  'telegram.test_mode': 'Dùng máy chủ thử nghiệm của Telegram (chỉ dành cho lập trình viên).',
  'telegram.schema_file':
    'Đường dẫn tới tệp api.tl riêng. Để trống để dùng bản đi kèm trong tệp thực thi.',
  'database.kind': 'sqlite = một tệp duy nhất, không cần cài gì. mysql = dùng máy chủ MySQL/MariaDB.',
  'server.port': 'Cổng của giao diện web. Đổi cổng cần khởi động lại ứng dụng.',
  'server.enable_webdav': 'Bật WebDAV để gắn ổ đĩa vào Windows Explorer, Finder, VLC…',
  'server.public_url':
    'Địa chỉ công khai dùng khi tạo liên kết chia sẻ, ví dụ https://disk.tenmien.com',
  'logging.level': 'Mức chi tiết của nhật ký. Chọn “trace” khi cần soi kỹ từng bước.',
  'security.session_days': 'Số ngày giữ phiên đăng nhập web.',
};

const NHOM_CAI_DAT = [
  ['storage', 'Lưu trữ & tải lên', '📦'],
  ['telegram', 'Telegram', '✈️'],
  ['database', 'Cơ sở dữ liệu', '🗄️'],
  ['server', 'Máy chủ web', '🌐'],
  ['logging', 'Nhật ký', '📋'],
  ['security', 'Bảo mật', '🔒'],
];

// Thứ tự hiển thị (những mục hay chỉnh nhất đứng trước).
const THU_TU_CAI_DAT = {
  storage: ['chunk_size', 'buffer_mode', 'browser_chunk_size', 'parallel_chunks',
            'memory_budget', 'download_cache_bytes', 'deduplicate',
            'trash_retention_days', 'upload_idle_timeout_seconds',
            'spool_directory', 'download_cache_directory'],
  telegram: ['backend', 'api_id', 'api_hash', 'channel_title', 'channel_username',
             'channel_id', 'channel_access_hash', 'obfuscated', 'connections_per_account',
             'request_timeout_seconds', 'layer', 'schema_file', 'device_model',
             'system_version', 'app_version', 'lang_code', 'test_mode', 'local_directory'],
  database: ['kind', 'sqlite_path', 'mysql_host', 'mysql_port', 'mysql_database',
             'mysql_user', 'mysql_password'],
  server: ['port', 'bind_address', 'public_url', 'enable_webdav', 'webdav_prefix',
           'worker_threads', 'idle_timeout_seconds', 'max_request_body_mb',
           'trust_proxy_headers'],
  logging: ['level', 'console', 'log_requests', 'file', 'max_file_bytes', 'max_files',
            'memory_records'],
  security: ['session_days', 'public_share_links', 'allow_registration',
             'password_iterations'],
};

// Các khoá mang giá trị byte — hiển thị kèm đơn vị cho dễ đọc.
const KHOA_BYTE = new Set(['chunk_size', 'browser_chunk_size', 'memory_budget',
                           'download_cache_bytes', 'max_file_bytes']);

function byteThanhChuoi(n) {
  n = Number(n) || 0;
  if (n === 0) return '0';
  const donVi = [[TB, 'TB'], [GB, 'GB'], [MB, 'MB'], [KB, 'KB']];
  for (const [co, ten] of donVi) {
    if (n >= co && n % co === 0) return (n / co) + ten;
  }
  for (const [co, ten] of donVi) {
    if (n >= co) return (n / co).toFixed(2).replace(/\.?0+$/, '') + ten;
  }
  return String(n);
}

const NHAN_CAI_DAT = {
  chunk_size: 'Kích thước mảnh', buffer_mode: 'Chế độ đệm',
  browser_chunk_size: 'Gói dữ liệu từ trình duyệt', parallel_chunks: 'Số mảnh song song',
  memory_budget: 'Giới hạn RAM cho vùng đệm', spool_directory: 'Thư mục tệp tạm',
  download_cache_bytes: 'Bộ đệm tải xuống', download_cache_directory: 'Thư mục bộ đệm',
  upload_idle_timeout_seconds: 'Thời gian chờ tối đa (giây)',
  trash_retention_days: 'Giữ thùng rác (ngày)', deduplicate: 'Khử trùng lặp',
  api_id: 'api_id', api_hash: 'api_hash', device_model: 'Tên thiết bị',
  system_version: 'Phiên bản hệ thống', app_version: 'Phiên bản ứng dụng',
  lang_code: 'Mã ngôn ngữ', layer: 'Layer TL', test_mode: 'Máy chủ thử nghiệm',
  obfuscated: 'Nguỵ trang kết nối', connections_per_account: 'Kết nối mỗi tài khoản',
  request_timeout_seconds: 'Thời gian chờ yêu cầu (giây)', channel_id: 'ID siêu nhóm',
  channel_access_hash: 'Access hash', channel_title: 'Tên siêu nhóm',
  channel_username: 'Tên người dùng nhóm', backend: 'Nơi lưu trữ',
  local_directory: 'Thư mục lưu nội bộ', schema_file: 'Tệp schema TL',
  extra_rsa_keys: 'Khoá RSA bổ sung',
  kind: 'Loại', sqlite_path: 'Đường dẫn tệp SQLite', mysql_host: 'Máy chủ MySQL',
  mysql_port: 'Cổng MySQL', mysql_user: 'Tài khoản MySQL', mysql_password: 'Mật khẩu MySQL',
  mysql_database: 'Tên cơ sở dữ liệu',
  bind_address: 'Địa chỉ lắng nghe', port: 'Cổng', worker_threads: 'Số luồng xử lý',
  max_request_body_mb: 'Giới hạn thân yêu cầu (MB)',
  idle_timeout_seconds: 'Thời gian chờ kết nối (giây)', public_url: 'Địa chỉ công khai',
  enable_webdav: 'Bật WebDAV', webdav_prefix: 'Tiền tố WebDAV',
  trust_proxy_headers: 'Tin tiêu đề proxy',
  level: 'Mức nhật ký', console: 'In ra màn hình', file: 'Tệp nhật ký',
  max_file_bytes: 'Kích thước tệp tối đa', max_files: 'Số tệp giữ lại',
  memory_records: 'Số dòng giữ trong RAM', log_requests: 'Ghi lại mọi yêu cầu',
  session_days: 'Số ngày giữ phiên', password_iterations: 'Số vòng băm mật khẩu',
  allow_registration: 'Cho tự đăng ký', public_share_links: 'Cho chia sẻ công khai',
};

async function napCaiDat() {
  const box = $('#khung-cai-dat');
  box.innerHTML = '<div class="panel"><div class="muted">Đang tải cài đặt…</div></div>';
  let kq;
  try { kq = await api('/api/settings'); }
  catch (err) {
    box.innerHTML = `<div class="panel"><div class="muted">Lỗi: ${thoat(err.message)}</div></div>`;
    return;
  }
  const cauHinh = kq.settings;
  box.innerHTML = '';

  const oInput = {};
  for (const [khoa, tieuDe, icon] of NHOM_CAI_DAT) {
    const nhom = cauHinh[khoa] || {};
    const luoi = el('div', { class: 'settings-grid' });

    // Sắp theo thứ tự đã định, các khoá lạ xếp cuối.
    const uuTien = THU_TU_CAI_DAT[khoa] || [];
    const cacKhoa = Object.keys(nhom).sort((a, b) => {
      const ia = uuTien.indexOf(a), ib = uuTien.indexOf(b);
      if (ia < 0 && ib < 0) return a.localeCompare(b);
      if (ia < 0) return 1;
      if (ib < 0) return -1;
      return ia - ib;
    });

    for (const k of cacKhoa) {
      const v = nhom[k];
      if (Array.isArray(v)) continue;
      const duong = khoa + '.' + k;
      const nhan = NHAN_CAI_DAT[k] || k;
      const help = MO_TA_CAI_DAT[duong];
      let input;

      if (typeof v === 'boolean') {
        input = el('input', { type: 'checkbox', checked: v ? 'checked' : null });
        luoi.appendChild(el('div', { class: 'setting' },
          el('label', { class: 'switch' }, input, el('span', {}, nhan)),
          help ? el('div', { class: 'help', text: help }) : null));
      } else if (k === 'buffer_mode') {
        input = el('select', { class: 'select' },
          el('option', { value: 'stream', selected: v === 'stream' ? 'selected' : null },
             'stream — ít RAM nhất (khuyên dùng)'),
          el('option', { value: 'memory', selected: v === 'memory' ? 'selected' : null },
             'memory — giữ trọn mảnh trong RAM'),
          el('option', { value: 'disk', selected: v === 'disk' ? 'selected' : null },
             'disk — ghi ra tệp tạm'));
        luoi.appendChild(el('div', { class: 'setting' }, el('label', { text: nhan }), input,
          help ? el('div', { class: 'help', text: help }) : null));
      } else if (k === 'backend') {
        input = el('select', { class: 'select' },
          el('option', { value: 'telegram', selected: v === 'telegram' ? 'selected' : null },
             'Telegram (lưu thật)'),
          el('option', { value: 'local', selected: v === 'local' ? 'selected' : null },
             'Nội bộ (chạy thử)'));
        luoi.appendChild(el('div', { class: 'setting' }, el('label', { text: nhan }), input,
          help ? el('div', { class: 'help', text: help }) : null));
      } else if (khoa === 'database' && k === 'kind') {
        input = el('select', { class: 'select' },
          el('option', { value: 'sqlite', selected: v === 'sqlite' ? 'selected' : null },
             'SQLite (tệp)'),
          el('option', { value: 'mysql', selected: v === 'mysql' ? 'selected' : null },
             'MySQL / MariaDB'));
        luoi.appendChild(el('div', { class: 'setting' }, el('label', { text: nhan }), input,
          help ? el('div', { class: 'help', text: help }) : null));
      } else if (k === 'level') {
        input = el('select', { class: 'select' },
          ...['trace', 'debug', 'info', 'warn', 'error'].map((x) =>
            el('option', { value: x, selected: v === x ? 'selected' : null }, x)));
        luoi.appendChild(el('div', { class: 'setting' }, el('label', { text: nhan }), input,
          help ? el('div', { class: 'help', text: help }) : null));
      } else {
        const laByte = KHOA_BYTE.has(k) && typeof v === 'number';
        input = el('input', {
          class: 'input',
          type: (typeof v === 'number' && !laByte) ? 'number' : 'text',
          value: laByte ? byteThanhChuoi(v) : String(v),
          placeholder: k === 'mysql_password' ? '(để trống nếu không đổi)' : '',
        });
        const phuChu = laByte
          ? el('div', { class: 'help',
                        text: `Bằng ${dungLuong(v)} (${v} byte). Nhập kèm đơn vị: KB, MB, GB.` })
          : null;
        luoi.appendChild(el('div', { class: 'setting' }, el('label', { text: nhan }), input,
          help ? el('div', { class: 'help', text: help }) : null, phuChu));
      }
      oInput[duong] = { input, goc: v };
    }

    box.appendChild(el('div', { class: 'panel settings-section' },
      el('div', { class: 'panel-head' }, el('h2', {}, `${icon}  ${tieuDe}`)),
      luoi));
  }

  const trangThai = el('div', { class: 'muted' });
  const nutLuu = el('button', { class: 'btn btn-primary', onclick: async () => {
    const body = {};
    for (const [duong, { input, goc }] of Object.entries(oInput)) {
      const [nhom, khoa] = duong.split('.');
      body[nhom] = body[nhom] || {};
      let giaTri;
      if (input.type === 'checkbox') giaTri = input.checked;
      else if (typeof goc === 'number' && input.type === 'number') giaTri = Number(input.value);
      else giaTri = input.value;
      if (khoa === 'mysql_password' && giaTri === '') continue;  // giữ mật khẩu cũ
      body[nhom][khoa] = giaTri;
    }
    nutLuu.disabled = true;
    trangThai.textContent = 'Đang lưu…';
    try {
      const kq2 = await api('/api/settings', { method: 'POST', body });
      trangThai.textContent = kq2.message;
      thongBao('Đã lưu cài đặt', kq2.message, 'ok', 6);
      capNhatThongKeNhanh();
    } catch (err) {
      trangThai.textContent = 'Lỗi: ' + err.message;
      thongBao('Không lưu được', err.message, 'err');
    } finally { nutLuu.disabled = false; }
  } }, 'Lưu cài đặt');

  box.appendChild(el('div', { class: 'panel' },
    el('div', { class: 'row' }, nutLuu,
      el('button', { class: 'btn btn-ghost', onclick: napCaiDat }, 'Nạp lại'), trangThai),
    el('div', { class: 'help', style: 'margin-top:10px' },
      `Tệp cấu hình: ${kq.config_path} · Schema TL: layer ${kq.schema_layer}, ` +
      `${kq.schema_constructors} hàm dựng`)));
}

/* ══════════════════════════════════════════════════════════════════════════
   Menu người dùng
   ══════════════════════════════════════════════════════════════════════════ */
$('#nut-nguoi-dung').addEventListener('click', (e) => {
  e.stopPropagation();
  const m = $('#menu-nguoi-dung');
  m.hidden = !m.hidden;
});

$('#menu-nguoi-dung').addEventListener('click', async (e) => {
  const nut = e.target.closest('[data-action]');
  if (!nut) return;
  $('#menu-nguoi-dung').hidden = true;
  const act = nut.dataset.action;

  if (act === 'dang-xuat') {
    try { await api('/api/auth/logout', { method: 'POST' }); } catch (err) {}
    location.reload();
    return;
  }
  if (act === 'doi-mat-khau') {
    const oCu = el('input', { class: 'input', type: 'password' });
    const oMoi = el('input', { class: 'input', type: 'password' });
    const oLai = el('input', { class: 'input', type: 'password' });
    const trangThai = el('div', { class: 'muted' });
    const than = el('div', {},
      el('div', { class: 'setting' }, el('label', { text: 'Mật khẩu hiện tại' }), oCu),
      el('div', { class: 'setting' }, el('label', { text: 'Mật khẩu mới' }), oMoi),
      el('div', { class: 'setting' }, el('label', { text: 'Nhập lại mật khẩu mới' }), oLai),
      trangThai);
    const nutOk = el('button', { class: 'btn btn-primary', onclick: async () => {
      if (oMoi.value !== oLai.value) {
        trangThai.textContent = 'Hai lần nhập mật khẩu mới chưa khớp.';
        return;
      }
      try {
        await api('/api/auth/password', { method: 'POST',
          body: { old_password: oCu.value, new_password: oMoi.value } });
        HopThoai.dong();
        thongBao('Đã đổi mật khẩu', 'Hãy đăng nhập lại.', 'ok', 4);
        setTimeout(() => location.reload(), 1500);
      } catch (err) { trangThai.textContent = 'Lỗi: ' + err.message; }
    } }, 'Đổi mật khẩu');
    HopThoai.mo('Đổi mật khẩu', than,
      [el('button', { class: 'btn btn-ghost', onclick: HopThoai.dong }, 'Huỷ'), nutOk]);
    return;
  }
  if (act === 'webdav') {
    const url = location.origin + '/webdav';
    const than = el('div', {},
      el('p', { style: 'margin:0' },
        'Gắn ổ đĩa vào máy tính để dùng như một thư mục bình thường, ' +
        'hoặc mở trực tiếp bằng VLC / Kodi / PotPlayer để xem phim không cần tải về.'),
      el('div', { class: 'setting' }, el('label', { text: 'Địa chỉ WebDAV' }),
         el('input', { class: 'input', readonly: 'readonly', value: url,
                       onclick: (ev) => ev.target.select() })),
      el('div', { class: 'kv-grid' },
        el('div', { class: 'kv' }, el('span', {}, 'Windows'),
           el('strong', {}, 'File Explorer → Ánh xạ ổ đĩa mạng')),
        el('div', { class: 'kv' }, el('span', {}, 'macOS'),
           el('strong', {}, 'Finder → Go → Connect to Server')),
        el('div', { class: 'kv' }, el('span', {}, 'Linux'),
           el('strong', {}, 'dav://… hoặc davfs2')),
        el('div', { class: 'kv' }, el('span', {}, 'Đăng nhập'),
           el('strong', {}, 'Dùng chính tài khoản web của bạn'))),
      el('p', { class: 'muted', style: 'margin:6px 0 0' },
        'Windows chỉ nhận WebDAV qua HTTPS khi truy cập từ máy khác; nếu dùng HTTP, ' +
        'hãy đặt ứng dụng sau một proxy có chứng chỉ.'));
    HopThoai.mo('Hướng dẫn WebDAV', than,
      [el('button', { class: 'btn btn-primary', onclick: HopThoai.dong }, 'Đã hiểu')]);
  }
});

/* ── Đóng hộp thoại ────────────────────────────────────────────────────── */
$('#nut-dong-hop-thoai').addEventListener('click', HopThoai.dong);
$('#lop-phu-hop-thoai').addEventListener('click', HopThoai.dong);

/* ── Khởi động ─────────────────────────────────────────────────────────── */
khoiDong();
