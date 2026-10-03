#pragma once

static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b6bcb">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>Modbus-Bridge</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 16 16%22%3E%3Crect width=%2216%22 height=%2216%22 rx=%223%22 fill=%22%230b6bcb%22/%3E%3Cpath d=%22M4 11V5l4 4 4-4v6%22 stroke=%22white%22 stroke-width=%221.6%22 fill=%22none%22/%3E%3C/svg%3E">
<style>
/* =====================================================================
   Farben und Maße (hell / dunkel)
   ===================================================================== */
:root {
  --bg: #f2f4f7;
  --card: #ffffff;
  --card-border: #e2e7ee;
  --fg: #18212b;
  --mut: #5d6977;
  --line: #e6eaf0;
  --input-bg: #f7f9fb;
  --acc: #0b6bcb;
  --acc-soft: rgba(11, 107, 203, .10);
  --ok: #17803a;
  --bad: #c62828;
  --warn: #9a5b00;
  --term-bg: #0f1419;
  --term-fg: #d6dde6;
  --shadow: 0 1px 2px rgba(16, 24, 40, .05);
  --radius: 12px;
  /* Seitenabstand wächst mit der Bildschirmbreite und berücksichtigt Notch/abgerundete Ecken */
  --gutter: clamp(12px, 1.6vw, 24px);
  --pad-left: max(var(--gutter), env(safe-area-inset-left));
  --pad-right: max(var(--gutter), env(safe-area-inset-right));
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0e1217;
    --card: #171c23;
    --card-border: #232a33;
    --fg: #e5eaef;
    --mut: #8f9bab;
    --line: #262e38;
    --input-bg: #11161c;
    --acc: #4ea1ff;
    --acc-soft: rgba(78, 161, 255, .14);
    --ok: #3fb950;
    --bad: #f47067;
    --warn: #e3a33b;
    --shadow: none;
  }
}

/* =====================================================================
   Grundlayout
   ===================================================================== */
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; text-size-adjust: 100%; }
body {
  margin: 0;
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  background: var(--bg);
  color: var(--fg);
  overflow-wrap: anywhere;  /* lange SSIDs, Topics, Hex-Werte brechen um statt die Seite zu verbreitern */
}
.ok { color: var(--ok); }
.bad { color: var(--bad); }
.warn { color: var(--warn); }
.mut { color: var(--mut); font-weight: 400; }
.hide { display: none !important; }
a { color: var(--acc); }
.hint { color: var(--mut); font-size: 12.5px; margin-top: 8px; line-height: 1.45; }

/* =====================================================================
   Kopfzeile und Reiter (bleiben beim Scrollen oben)
   ===================================================================== */
.top {
  position: sticky;
  top: 0;
  z-index: 10;
  background: var(--card);
  border-bottom: 1px solid var(--card-border);
}
header {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: max(10px, env(safe-area-inset-top)) var(--pad-right) 6px var(--pad-left);
}
header h1 { font-size: 16px; margin: 0; font-weight: 650; line-height: 1.25; }
header h1 .subtitle { color: var(--mut); font-weight: 500; margin-left: 6px; }
header .sp { flex: 1; }
/* Statusleiste in der Kopfzeile: auf schmalen Bildschirmen eigene Zeile, waagrecht scrollbar */
.status-badges { display: flex; gap: 6px; flex-wrap: wrap; justify-content: flex-end; }
.status-badges .pill { font-weight: 550; }
@media (max-width: 900px) {
  header { flex-wrap: wrap; }
  header .sp { display: none; }
  .status-badges { width: 100%; flex-wrap: nowrap; justify-content: flex-start; overflow-x: auto; scrollbar-width: none; }
}
nav {
  display: flex;
  gap: 2px;
  padding: 0 var(--pad-right) 0 var(--pad-left);
  overflow-x: auto;
  -webkit-overflow-scrolling: touch;
  scrollbar-width: none;
}
nav::-webkit-scrollbar { display: none; }
nav button {
  flex: 0 0 auto;
  border: 0;
  border-bottom: 2px solid transparent;
  background: none;
  color: var(--mut);
  padding: 10px 12px 9px;
  font: inherit;
  font-weight: 500;
  cursor: pointer;
  white-space: nowrap;
}
nav button:hover { color: var(--fg); }
nav button.on { color: var(--acc); border-bottom-color: var(--acc); font-weight: 600; }
/* Untermenü: zweite, etwas abgesetzte Leiste mit kompakten "Pillen" */
.subnav {
  gap: 6px;
  padding-top: 8px;
  padding-bottom: 8px;
  background: var(--bg);
  border-top: 1px solid var(--card-border);
}
.subnav button {
  border: 1px solid var(--card-border);
  border-radius: 99px;
  padding: 5px 14px;
  font-size: 13px;
  background: var(--card);
}
.subnav button.on { border-color: var(--acc); background: var(--acc-soft); color: var(--acc); }

/* =====================================================================
   Raster: 12 Spalten, volle Breite; Karten einer Reihe sind gleich hoch
   ===================================================================== */
main {
  width: 100%;
  padding: var(--gutter) var(--pad-right) max(var(--gutter), env(safe-area-inset-bottom)) var(--pad-left);
}
main > section > * + * { margin-top: var(--gutter); }
.layout {
  display: grid;
  grid-template-columns: repeat(12, minmax(0, 1fr));
  gap: var(--gutter);
  align-items: stretch;
}
.layout > * { grid-column: span 12; min-width: 0; }
.layout > .span-3 { grid-column: span 3; }
.layout > .span-4 { grid-column: span 4; }
.layout > .span-6 { grid-column: span 6; }
.layout > .span-8 { grid-column: span 8; }
/* mehrere Karten untereinander in einer Rasterzelle */
.stack { display: flex; flex-direction: column; gap: var(--gutter); }
.stack > .card:last-child { flex: 1; }

/* =====================================================================
   Karten
   ===================================================================== */
.card {
  display: flex;
  flex-direction: column;
  background: var(--card);
  border: 1px solid var(--card-border);
  border-radius: var(--radius);
  box-shadow: var(--shadow);
  padding: 16px 18px;
  min-width: 0;
}
.card-head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 8px 12px;
  flex-wrap: wrap;
  margin-bottom: 10px;
}
.card-head h2, .card > h2 { font-size: 14.5px; margin: 0; font-weight: 650; }
.card-meta { color: var(--mut); font-size: 12.5px; display: inline-flex; align-items: center; gap: 6px; flex-wrap: wrap; }
/* Fußzeile mit Aktionen: sitzt immer am unteren Kartenrand */
.card-foot {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
  margin-top: auto;
  padding-top: 14px;
}
.card-foot .hint { margin-top: 0; }
.subsection { border-top: 1px dashed var(--line); margin-top: 14px; padding-top: 2px; }

/* Kennzahl-Kacheln der Übersicht */
.kpi { padding: 14px 18px; }
.kpi-label { color: var(--mut); font-size: 12.5px; font-weight: 500; }
.kpi-value { font-size: 26px; font-weight: 650; line-height: 1.2; margin-top: 4px; font-variant-numeric: tabular-nums; }
.kpi-value small { font-size: 14px; font-weight: 500; color: var(--mut); margin-left: 3px; }
.kpi-sub { color: var(--mut); font-size: 12.5px; margin-top: 2px; min-height: 1.5em; }

/* Aktionsleiste unter Formularen: bleibt beim Scrollen am unteren Rand sichtbar */
.actions {
  position: sticky;
  bottom: max(8px, env(safe-area-inset-bottom));
  z-index: 5;
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 8px 16px;
  flex-wrap: wrap;
  background: var(--card);
  border: 1px solid var(--card-border);
  border-radius: var(--radius);
  box-shadow: 0 4px 16px rgba(0, 0, 0, .12);
  padding: 10px 14px;
}
.actions .hint { margin: 0 auto 0 0; }

/* =====================================================================
   Tabellen und Schlüssel/Wert-Listen
   ===================================================================== */
.table-wrap { overflow-x: auto; -webkit-overflow-scrolling: touch; }
table { border-collapse: collapse; width: 100%; }
td, th {
  padding: 6px 10px;
  vertical-align: middle;
  text-align: right;
  border-bottom: 1px solid var(--line);
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}
tr:last-child td { border-bottom: 0; }
td:first-child, th:first-child { text-align: left; padding-left: 0; color: var(--mut); white-space: normal; }
td:last-child, th:last-child { padding-right: 0; }
th { font-weight: 600; color: var(--mut); font-size: 12.5px; }
/* Messwerttabelle: feste Zahlenspalten, Beschriftung nimmt den Rest */
.measure-table th:not(:first-child), .measure-table td:not(:first-child) { width: 15%; }
.value-table td:first-child { overflow-wrap: normal; hyphens: auto; -webkit-hyphens: auto; }
.value-table td:nth-child(2) { color: var(--fg); font-weight: 500; }
.value-table td:nth-child(3) { color: var(--mut); font-size: 12px; width: 1%; }
.pinmap td { text-align: left; white-space: normal; }
.pinmap td:first-child { width: 170px; }
.kv { display: grid; grid-template-columns: minmax(120px, max-content) 1fr; gap: 5px 16px; }
.kv span:nth-child(odd) { color: var(--mut); }
/* Umschalter Intervall 1/2 in der GoodWe-Wertetabelle */
.poll-toggle { display: inline-flex; border: 1px solid var(--card-border); border-radius: 7px; overflow: hidden; }
.poll-toggle button {
  border: 0; background: transparent; color: var(--mut); font: inherit; font-size: 12px; font-weight: 600;
  padding: 1px 9px; cursor: pointer; min-height: 0;
}
.poll-toggle button.on { background: var(--acc); color: #fff; }
.poll-toggle button:disabled { cursor: not-allowed; }
.poll-toggle.locked button.on { background: var(--mut); }
.value-table td:nth-child(4) { width: 1%; }
/* feste Spaltenbreiten, damit die Tabelle bei wechselnden Werten (z. B. 1e20) nicht springt */
.lumel-table { table-layout: fixed; width: 100%; min-width: 660px; }
.lumel-table td:first-child { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lumel-table td:nth-child(2) { color: var(--fg); font-weight: 500; }
.lumel-table td:nth-child(3) { color: var(--mut); font-size: 12px; font-weight: 400; width: auto; }
.lumel-table td:nth-child(n+4) { color: var(--mut); font-size: 12px; }
.lumel-table td:last-child { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; white-space: nowrap; overflow: visible; }
.identify-btn { margin-left: 8px; padding: 1px 10px; min-height: 0; font-size: 12px; vertical-align: 1px; }
.card-meta .card-switch { display: inline-flex; align-items: center; margin: 0; font-size: 12.5px; line-height: 1; color: var(--mut); gap: 5px; }
.card-meta .card-switch input[type=checkbox] { width: 15px; height: 15px; margin: 0; }
.card-off h2 { color: var(--mut); }
.kpi-label .pill { margin-left: 6px; font-size: 11.5px; padding: 0 8px; vertical-align: 1px; }
.pill {
  display: inline-block;
  padding: 2px 10px;
  border-radius: 99px;
  font-size: 12.5px;
  font-weight: 600;
  background: var(--bg);
  white-space: nowrap;
}

/* =====================================================================
   Formulare
   ===================================================================== */
label { display: block; margin: 12px 0 4px; color: var(--mut); font-size: 12.5px; font-weight: 500; }
input, select, textarea {
  width: 100%;
  min-height: 36px;
  padding: 7px 10px;
  border: 1px solid var(--card-border);
  border-radius: 8px;
  background: var(--input-bg);
  color: var(--fg);
  font: inherit;
}
input:focus, select:focus, textarea:focus { outline: none; border-color: var(--acc); box-shadow: 0 0 0 3px var(--acc-soft); }
input[type=checkbox] { width: 16px; height: 16px; min-height: 0; margin: 0 10px 0 0; flex: 0 0 auto; accent-color: var(--acc); }
.chk { display: flex; align-items: center; color: var(--fg); margin-top: 14px; font-size: 14px; font-weight: 400; }
.row { display: grid; grid-template-columns: 1fr 1fr; gap: 0 12px; }
.row3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 0 12px; }
.inline-row { display: flex; gap: 8px; align-items: stretch; }
.inline-row .btn { flex: 0 0 auto; }
textarea { font: 12.5px/1.4 ui-monospace, Menlo, Consolas, monospace; resize: vertical; }
textarea.masked { -webkit-text-security: disc; text-security: disc; }
.lrow { display: flex; align-items: flex-end; justify-content: space-between; gap: 8px; }
.fbtn { margin: 12px 0 4px; color: var(--acc); font-size: 12.5px; font-weight: 600; cursor: pointer; white-space: nowrap; }
.fbtn:hover { text-decoration: underline; }
/* eigene Dateiauswahl statt des Browser-Standardknopfs */
.file-pick { display: flex; align-items: center; gap: 10px; margin: 4px 0 0; cursor: pointer; color: var(--fg); font-weight: 400; }
.file-name { color: var(--mut); font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

/* Knöpfe */
.btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-height: 36px;
  border: 1px solid transparent;
  background: var(--acc);
  color: #fff;
  padding: 7px 16px;
  border-radius: 8px;
  font: inherit;
  font-weight: 600;
  cursor: pointer;
  text-decoration: none;
  white-space: nowrap;
}
.btn:hover { filter: brightness(1.06); }
.btn.sec { background: transparent; color: var(--fg); border-color: var(--card-border); }
.btn.sec:hover { background: var(--bg); filter: none; }
.btn.danger { background: transparent; color: var(--bad); border-color: var(--bad); }
.btn.small { min-height: 30px; padding: 4px 12px; font-size: 13px; font-weight: 500; }
.btn:disabled { opacity: .45; cursor: default; filter: none; }

/* Passwort anzeigen/verbergen (Auge) */
.pw { position: relative; }
.pw input, .pw textarea { padding-right: 42px; }
.eye {
  position: absolute;
  right: 4px;
  top: 50%;
  transform: translateY(-50%);
  border: 0;
  background: none;
  color: var(--mut);
  cursor: pointer;
  padding: 6px;
  line-height: 0;
  border-radius: 6px;
}
.eye:hover { color: var(--fg); }
.eye svg { width: 20px; height: 20px; }
.pw .eye.ta { top: 8px; transform: none; }

/* =====================================================================
   Konsole
   ===================================================================== */
#t-co .card { padding: 10px 12px; }
.console-toolbar { display: flex; flex-wrap: wrap; align-items: center; gap: 6px 14px; margin-bottom: 8px; }
.console-toolbar h2 { margin: 0; font-size: 14.5px; }
.console-toolbar .chk { margin: 0; font-size: 13px; }
.console-toolbar .sp { flex: 1; }
#consoleOutput {
  margin: 0;
  height: 60vh;  /* Startwert; fitConsoleHeight() passt die Höhe an das Fenster an */
  min-height: 160px;
  overflow: auto;
  -webkit-overflow-scrolling: touch;
  padding: 10px 12px;
  border-radius: 8px;
  background: var(--term-bg);
  color: var(--term-fg);
  font: 12.5px/1.45 ui-monospace, Menlo, Consolas, monospace;
  white-space: pre-wrap;
  word-break: break-all;
  outline: none;
}
#consoleOutput:focus { box-shadow: 0 0 0 2px var(--acc); }
#consoleOutput .log-error { color: #ff7b72; }
#consoleOutput .log-warn { color: #e3b341; }
#consoleOutput .log-command { color: #79c0ff; font-weight: 600; }
#consoleOutput .log-modbus { color: #a5d6a7; }
.console-input { display: flex; gap: 6px; margin-top: 8px; }
.console-input input { min-height: 32px; padding: 5px 10px; font: 13px ui-monospace, Menlo, Consolas, monospace; }

/* =====================================================================
   Toast-Meldung
   ===================================================================== */
#toast {
  position: fixed;
  bottom: max(16px, env(safe-area-inset-bottom));
  left: 50%;
  transform: translateX(-50%);
  background: var(--fg);
  color: var(--bg);
  padding: 10px 16px;
  border-radius: 10px;
  opacity: 0;
  transition: .2s;
  pointer-events: none;
  max-width: calc(100vw - 32px);
  z-index: 20;
}
#toast.on { opacity: 1; }

/* =====================================================================
   Anpassung an die Bildschirmgröße
   ===================================================================== */
/* Tablets / kleine Laptops: Viertel-Kacheln werden zu Hälften, 8+4 wird untereinander */
@media (max-width: 1100px) {
  .layout > .span-3 { grid-column: span 6; }
  .layout > .span-4, .layout > .span-8 { grid-column: span 12; }
  .layout > .span-4.stack { flex-direction: row; flex-wrap: wrap; }
  .layout > .span-4.stack > .card { flex: 1 1 320px; }
}
/* Smartphones: alles einspaltig */
@media (max-width: 720px) {
  .layout > .span-6 { grid-column: span 12; }
  header h1 .subtitle { display: block; margin-left: 0; font-size: 12.5px; }
  .row, .row3 { grid-template-columns: 1fr; }
  .kv { grid-template-columns: 1fr; gap: 0; }
  .kv span:nth-child(odd) { font-size: 12px; margin-top: 7px; }
  .kpi-value { font-size: 22px; }
  td, th { padding: 6px 6px; }
  .actions { justify-content: stretch; }
  .actions .btn { flex: 1; }
  #consoleOutput { font-size: 11.5px; }
}
/* sehr schmale Smartphones: Kennzahl-Kacheln bleiben als 2x2-Raster, nur kompakter */
@media (max-width: 420px) {
  .kpi { padding: 10px 12px; }
  .kpi-value { font-size: 19px; }
  .kpi-label, .kpi-sub { font-size: 11.5px; }
}
/* Touch-Geräte: größere Ziele; Eingabefelder mit 16 px, sonst zoomt Safari beim Antippen hinein */
@media (pointer: coarse) {
  input, select, textarea, .console-input input { font-size: 16px; min-height: 42px; }
  .btn { min-height: 42px; }
  .btn.small { min-height: 34px; }
  nav button { padding: 12px 12px 11px; }
  .subnav button { padding: 7px 14px; }
  input[type=checkbox] { width: 20px; height: 20px; }
}
</style>
</head>
<body>

<!-- ========== Kopfzeile und Reiter ========== -->
<div class="top">
  <header>
    <h1>Modbus-Bridge <span class="subtitle"><span class="device-name">GoodWe</span> &rarr; Lumel N43</span></h1>
    <span class="sp"></span>
    <div class="status-badges">
      <span id="hdr" class="pill">&hellip;</span>
      <span id="hdrModbus" class="pill">Modbus</span>
      <span id="hdrMqtt" class="pill">MQTT</span>
      <span id="hdrVpn" class="pill">VPN</span>
      <span id="hdrHeapUsed" class="pill" title="Belegter Heap">Heap &ndash;</span>
      <span id="hdrHeap" class="pill" title="Freier Heap">Free Heap &ndash;</span>
      <span id="hdrClock" class="pill" title="Datum und Uhrzeit des ESP (NTP)">&ndash;</span>
    </div>
  </header>
  <!-- Hauptleiste -->
  <nav id="nav">
    <button data-t="ov" class="on">&Uuml;bersicht</button>
    <button data-t="gw">GoodWe</button>
    <button data-t="lu">Lumel</button>
    <button data-group="system">System</button>
  </nav>
  <!-- Untermenü von "System" (nur sichtbar, wenn ein System-Reiter offen ist) -->
  <nav id="subnav" class="subnav hide">
    <button data-t="sy">Allgemein</button>
    <button data-t="co">Konsole</button>
    <button data-t="mq">MQTT</button>
    <button data-t="wl">WLAN</button>
    <button data-t="vp">VPN</button>
    <button data-t="mb">Modbus</button>
    <button data-t="gp">GPIO</button>
  </nav>
</div>

<main>

<!-- ========== Reiter: Übersicht ========== -->
<section id="t-ov">
  <!-- Kennzahl-Kacheln -->
  <div class="layout">
    <div class="card kpi span-3">
      <div class="kpi-label">Netzleistung</div>
      <div class="kpi-value" id="kpiGrid">–</div>
      <div class="kpi-sub" id="kpiGridSub">&nbsp;</div>
    </div>
    <div class="card kpi span-3">
      <div class="kpi-label">PV-Leistung</div>
      <div class="kpi-value" id="kpiPv">–</div>
      <div class="kpi-sub" id="kpiPvSub">&nbsp;</div>
    </div>
    <div class="card kpi span-3">
      <div class="kpi-label">Batterie<span id="kpiBatteryState" class="pill hide"></span></div>
      <div class="kpi-value" id="kpiBattery">–</div>
      <div class="kpi-sub" id="kpiBatterySub">&nbsp;</div>
    </div>
    <div class="card kpi span-3">
      <div class="kpi-label">Verbindung <span class="device-name">GoodWe</span></div>
      <div class="kpi-value" id="kpiLink">–</div>
      <div class="kpi-sub" id="kpiLinkSub">&nbsp;</div>
    </div>
  </div>

  <!-- Messwerte links, Status und GoodWe rechts -->
  <div class="layout">
    <div class="card span-8">
      <div class="card-head"><h2>Messwerte</h2><span class="card-meta">Ausgabe als Lumel N43</span></div>
      <div class="table-wrap">
        <table class="measure-table">
          <thead><tr><th>Gr&ouml;&szlig;e</th><th>L1</th><th>L2</th><th>L3</th><th>Gesamt</th></tr></thead>
          <tbody id="mt"></tbody>
        </table>
      </div>
      <div class="card-foot"><span class="hint">Vorzeichen: + = Bezug aus dem Netz (bei aktivierter Vorzeichen-Umkehr)</span></div>
    </div>
    <div class="span-4 stack">
      <div class="card"><div class="card-head"><h2>Status</h2></div><div class="kv" id="stat"></div></div>
      <div class="card"><div class="card-head"><h2 class="device-name">GoodWe</h2></div><div class="kv" id="gw"></div></div>
    </div>
  </div>

  <!-- Statistik der beiden RS485-Ports -->
  <div class="layout" id="portstat"></div>
</section>

<!-- ========== Reiter: Modbus ========== -->
<section id="t-mb" class="hide">
  <div class="layout">
    <div class="card span-6">
      <div class="card-head"><h2 class="device-name">GoodWe</h2><span class="card-meta">Master</span></div>
      <label>Anbindung des GoodWe</label>
      <select id="gwTransport" onchange="updateTransportVisibility()">
        <option value="rtu">RS485 &ndash; RTU-Port mit Rolle &bdquo;GoodWe&ldquo;</option>
        <option value="tcp">Modbus TCP &ndash; z.&nbsp;B. RTU-zu-TCP-Gateway</option>
      </select>
      <div id="tcpbox" class="hide subsection">
        <div class="row3">
          <div><label>Gateway (IP/Host)</label><input id="gwTcpHost" placeholder="192.168.178.50"></div>
          <div><label>TCP-Port</label><input id="gwTcpPort" type="number" min="1" max="65535"></div>
          <div><label>Unit-ID</label><input id="gwTcpUnit" type="number" min="0" max="255"></div>
        </div>
        <div class="hint">Unit-ID = Modbus-Adresse des GoodWe hinter dem Gateway (Standard 247). Liegt das Gateway in einem anderen Netz, wird es &uuml;ber den WireGuard-Tunnel erreicht (Reiter VPN).</div>
      </div>
      <label>Datenquelle</label>
      <select id="gwSource">
        <option value="meter">Smart-Meter am Netzanschluss (36000ff)</option>
        <option value="inverter">Wechselrichter-Ausgang (35100ff)</option>
      </select>
      <div class="row">
        <div><label>Intervall 1 &ndash; schnelle Werte (ms)</label><input id="gwPollMs" type="number" min="100" max="60000"></div>
        <div><label>Intervall 2 &ndash; alle Register (ms)</label><input id="gwSlowPollMs" type="number" min="1000" max="3600000" step="100"></div>
      </div>
      <label>Antwort-Timeout (ms)</label><input id="gwTimeoutMs" type="number" min="50" max="5000">
      <div class="hint">Welche Werte in Intervall 1 gelesen werden, legst du auf dem Reiter <a href="#gw" onclick="document.querySelector('[data-t=gw]').click()">GoodWe</a> fest. Werte f&uuml;r die Lumel-Emulation laufen immer in Intervall 1.</div>
      <label class="chk"><input type="checkbox" id="gwInvertSign">Vorzeichen umkehren (GoodWe: + = Einspeisung &rarr; Z&auml;hler: + = Bezug)</label>
    </div>
    <div class="card span-6">
      <div class="card-head"><h2>Lumel N43</h2><span class="card-meta">Slave</span></div>
      <label class="chk"><input type="checkbox" id="lumelWordSwap">Float-Wortreihenfolge tauschen (Low-Word zuerst)</label>
      <label class="chk"><input type="checkbox" id="lumelSilentOnStale">Ohne aktuelle GoodWe-Daten nicht antworten</label>
      <label class="chk"><input type="checkbox" id="lumelNoUndefined"><span>Nicht definierte Werte ersetzen: Leistungsfaktor&nbsp;=&nbsp;1, tg&nbsp;&phi;&nbsp;=&nbsp;0 statt 1e20</span></label>
      <div class="hint">Der echte N43 sendet 1e20, wenn P bzw. S fast 0 ist. Nur aktivieren, wenn das Auswerteger&auml;t damit nicht zurechtkommt.</div>
      <label>Daten gelten als veraltet nach (s)</label>
      <input id="staleSec" type="number" min="2" max="3600">
      <label class="chk"><input type="checkbox" id="testMode">Testmodus: feste Werte statt GoodWe ausgeben</label>
      <div class="row">
        <div><label>Test-Wirkleistung gesamt (W)</label><input id="testPowerW" type="number" step="1"></div>
        <div><label>Test-Spannung (V)</label><input id="testVoltage" type="number" step="0.1"></div>
      </div>
    </div>
  </div>
  <div class="layout">
    <div class="card span-6">
      <div class="card-head"><h2>Modbus-TCP-Bridge</h2><span class="card-meta">Server</span></div>
      <label class="chk"><input type="checkbox" id="bridgeEnabled"><span>Anfragen aus dem lokalen Netz an den <span class="device-name">GoodWe</span> durchreichen</span></label>
      <label>TCP-Port</label>
      <input id="bridgePort" type="number" min="1" max="65535">
      <div class="hint">Jede Anfrage (alle Funktionscodes) geht unver&auml;ndert an den GoodWe &ndash; ohne Zwischenspeicher, auch Exceptions kommen direkt vom Ger&auml;t. Unit-ID 0 oder 255 wird durch die Adresse des GoodWe ersetzt. Antwortet er nicht, meldet die Bridge Exception 0B.</div>
    </div>
    <div class="card span-6">
      <div class="card-head"><h2>Bridge-Status</h2></div>
      <div class="kv" id="bridgestat"></div>
    </div>
  </div>
  <div class="card-head" style="margin:4px 2px 0"><h2>RS485-Schnittstellen</h2></div>
  <div class="layout" id="portcfg"></div>
  <div class="actions">
    <span class="hint">&Auml;nderungen an Rolle oder Schnittstelle l&ouml;sen einen Neustart aus. Testmodus und Lumel-Optionen wirken sofort.</span>
    <button class="btn" onclick="saveModbus()">Speichern</button>
  </div>
</section>

<!-- ========== Reiter: GoodWe (alle Werte) ========== -->
<!-- ========== Reiter: Lumel (ausgegebene Register der Simulation) ========== -->
<section id="t-lu" class="hide">
  <div class="layout">
    <div class="card span-12">
      <div class="card-head"><h2>Lumel N43 &ndash; ausgegebene Register</h2><span class="card-meta" id="lumelInfo"></span></div>
      <div class="hint">So antwortet die Simulation gerade auf Leseanfragen. Messwerte stehen als Float in 7500&nbsp;ff. (ein 32-Bit-Register je Wert) und als 16-Bit-Paare in 7000&nbsp;ff. (High-Word zuerst) bzw. 6000&nbsp;ff. (Low-Word zuerst). Registerbelegung wie beim echten N43.</div>
    </div>
  </div>
  <div class="layout">
    <div class="card span-8">
      <div class="card-head"><h2>Messwerte</h2><span class="card-meta">7500&ndash;7574</span></div>
      <div class="table-wrap"><table class="value-table lumel-table">
        <colgroup><col><col style="width:10em"><col style="width:7em"><col style="width:4.5em"><col style="width:8em"><col style="width:7.5rem"></colgroup>
        <thead><tr><th>Inhalt</th><th>Wert</th><th>Einheit</th><th>7500</th><th>7000 / 6000</th><th>Hex</th></tr></thead>
        <tbody id="lumelValues"></tbody>
      </table></div>
    </div>
    <div class="card span-4">
      <div class="card-head"><h2>Konfiguration</h2><span class="card-meta">4000&ndash;4066</span></div>
      <div class="table-wrap"><table class="value-table">
        <thead><tr><th>Inhalt</th><th>Wert</th><th>Register</th></tr></thead>
        <tbody id="lumelConfig"></tbody>
      </table></div>
      <div class="hint">Nicht aufgef&uuml;hrte Register liefern 0. Schreibzugriffe werden quittiert, aber nicht gespeichert.</div>
    </div>
  </div>
</section>

<section id="t-gw" class="hide">
  <div class="layout">
    <div class="card span-12">
      <div class="card-head"><h2 id="gwtitle">Alle GoodWe-Werte</h2><span class="card-meta" id="gwinfo"></span></div>
      <div class="hint" id="gwPollInfo"></div>
    </div>
  </div>
  <div class="layout" id="gwBlocks"></div>
</section>

<!-- ========== Reiter: Konsole (Live-Log per WebSocket) ========== -->
<section id="t-co" class="hide">
  <div class="card">
    <div class="console-toolbar">
      <h2>Konsole</h2>
      <span id="consoleState" class="pill">getrennt</span>
      <span class="sp"></span>
      <label class="chk"><input type="checkbox" id="consoleTrace" onchange="setModbusTrace(this.checked)">Modbus-Verkehr</label>
      <label class="chk"><input type="checkbox" id="consoleAutoscroll" checked>Mitlaufen</label>
      <button class="btn sec small" onclick="clearConsoleView()">Leeren</button>
      <button class="btn sec small" onclick="downloadConsoleLog()">Speichern</button>
    </div>
    <pre id="consoleOutput" tabindex="0" aria-label="Konsolenausgabe"
         title="Strg+A (Mac: Cmd+A) markiert den gesamten Konsoleninhalt"></pre>
    <form class="console-input" onsubmit="sendConsoleCommand(event)">
      <input id="consoleCommand" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false"
             enterkeyhint="send" placeholder="Befehl, z. B. help, status, trace on  (&uarr;/&darr; = Verlauf)">
      <button class="btn small" type="submit">Senden</button>
    </form>
  </div>
</section>

<!-- ========== Reiter: Settings / GPIO ========== -->
<section id="t-gp" class="hide">
  <div class="layout" id="gpiocfg"></div>
  <div class="layout">
    <div class="card span-12">
      <div class="card-head"><h2 id="gpioHintTitle">Hinweise zu den GPIOs</h2></div>
      <div class="table-wrap">
        <table class="pinmap"><tbody id="gpioHints"></tbody></table>
      </div>
    </div>
  </div>
  <div class="actions">
    <span class="hint">Die neue Zuordnung wird nach einem Neustart aktiv.</span>
    <button class="btn" onclick="saveGpio()">Speichern &amp; neu starten</button>
  </div>
</section>

<!-- ========== Reiter: MQTT ========== -->
<section id="t-mq" class="hide">
  <div class="layout">
    <div class="card span-6">
      <div class="card-head"><h2>MQTT-Broker</h2></div>
      <label class="chk"><input type="checkbox" id="mqttEnabled">MQTT aktiv</label>
      <div class="row">
        <div><label>Broker (Host/IP)</label><input id="mqttHost" placeholder="z.B. 192.168.178.10"></div>
        <div><label>Port</label><input id="mqttPort" type="number" min="1" max="65535"></div>
      </div>
      <div class="row">
        <div><label>Benutzer</label><input id="mqttUser" autocomplete="off"></div>
        <div><label>Passwort</label><input id="mqttPass" type="password" placeholder="kein Passwort" autocomplete="new-password"></div>
      </div>
      <div class="row">
        <div><label>Basis-Topic</label><input id="mqttBase"></div>
        <div><label>Sendeintervall (s)</label><input id="mqttIntervalSec" type="number" min="1" max="3600"></div>
      </div>
      <label class="chk"><input type="checkbox" id="mqttSingleTopics">Zus&auml;tzlich jeden Wert als eigenes Topic senden</label>
      <label class="chk"><input type="checkbox" id="mqttTls" onchange="updateTlsVisibility(true)">TLS verwenden (MQTTS)</label>

      <!-- TLS-Optionen (nur sichtbar, wenn TLS aktiv ist) -->
      <div id="tlsbox" class="hide subsection">
        <label>Serverzertifikat</label>
        <select id="mqttTlsMode" onchange="updateTlsVisibility(false)">
          <option value="ca">Mit eigenem CA-Zertifikat pr&uuml;fen (z.&nbsp;B. eigener Broker)</option>
          <option value="bundle">Mit &ouml;ffentlichen Zertifizierungsstellen pr&uuml;fen (Cloud-Broker)</option>
          <option value="none">Nicht pr&uuml;fen &ndash; nur verschl&uuml;sseln (unsicher)</option>
        </select>
        <div id="cabox">
          <div class="lrow">
            <label>CA-Zertifikat (PEM)</label>
            <label class="fbtn">Datei laden<input type="file" accept=".pem,.crt,.cer" onchange="loadPemFile(this,'mqttCa')" hidden></label>
          </div>
          <textarea id="mqttCa" rows="4" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----"></textarea>
          <div class="hint">Zertifikat der Zertifizierungsstelle, die das Broker-Zertifikat ausgestellt hat. Der Broker-Name muss zum Zertifikat passen.</div>
        </div>
        <div class="lrow">
          <label>Client-Zertifikat (PEM, optional)</label>
          <label class="fbtn">Datei laden<input type="file" accept=".pem,.crt,.cer" onchange="loadPemFile(this,'mqttCert')" hidden></label>
        </div>
        <textarea id="mqttCert" rows="3" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----"></textarea>
        <div class="lrow">
          <label>Privater Schl&uuml;ssel (PEM, optional)</label>
          <label class="fbtn">Datei laden<input type="file" accept=".pem,.key" onchange="loadPemFile(this,'mqttKey')" hidden></label>
        </div>
        <textarea id="mqttKey" class="secret masked" rows="3" spellcheck="false" placeholder="-----BEGIN PRIVATE KEY-----"></textarea>
      </div>
    </div>
    <div class="span-6 stack">
      <div class="card">
        <div class="card-head"><h2>Verbindung</h2></div>
        <div class="kv" id="mqstat"></div>
      </div>
      <div class="card">
        <div class="card-head"><h2>Home Assistant</h2></div>
        <label class="chk"><input type="checkbox" id="mqttDiscovery">Auto-Discovery aktiv</label>
        <label>Discovery-Prefix</label>
        <input id="mqttDiscPrefix">
        <div class="hint">In Home Assistant erscheinen zwei Ger&auml;te: <b>GoodWe &lt;Modell&gt;</b> mit allen Messwerten und die <b>Modbus-Bridge</b> mit Diagnosewerten. Werte, die der Wechselrichter nicht liefert (z.&nbsp;B. BMS ohne Batterie), werden nicht angelegt.</div>
        <div class="card-foot"><button class="btn sec" onclick="resendDiscovery()">Discovery neu senden</button></div>
      </div>
      <div class="card">
        <div class="card-head"><h2>Topics</h2></div>
        <div class="kv" id="mqtopics"></div>
      </div>
    </div>
  </div>
  <div class="actions">
    <span class="hint">MQTT-&Auml;nderungen wirken sofort, ohne Neustart.</span>
    <button class="btn" onclick="saveMqtt()">Speichern</button>
  </div>
</section>

<!-- ========== Reiter: WLAN ========== -->
<section id="t-wl" class="hide">
  <div class="layout">
    <div class="card span-6">
      <div class="card-head"><h2>WLAN-Zugangsdaten</h2></div>
      <label>SSID</label>
      <div class="inline-row">
        <input id="wifiSsid" list="ssids" autocomplete="off">
        <button class="btn sec" onclick="scanWifiNetworks()">Suchen</button>
      </div>
      <datalist id="ssids"></datalist>
      <div id="scanres" class="hint"></div>
      <label>Passwort</label>
      <input id="wifiPass" type="password" placeholder="kein Passwort (offenes WLAN)" autocomplete="new-password">
      <div class="row">
        <div><label>Hostname (&lt;name&gt;.local)</label><input id="hostname"></div>
        <div><label>Passwort Konfigurations-AP</label><input id="apPass" type="password" placeholder="min. 8 Zeichen" autocomplete="new-password"></div>
      </div>
      <div class="card-foot"><button class="btn" onclick="saveWifi()">Speichern &amp; neu starten</button></div>
    </div>
    <div class="card span-6">
      <div class="card-head"><h2>Verbindung</h2></div>
      <div class="kv" id="wstat"></div>
      <label>Max. Sendeleistung (dBm, 0 = Maximum 20 dBm)</label>
      <div class="inline-row">
        <input id="wifiTxPower" type="number" min="0" max="20" step="0.25">
        <button class="btn sec" onclick="saveWifiTxPower()">&Uuml;bernehmen</button>
      </div>
      <div class="hint">Wirkt sofort, ohne Neustart. Kleine ESP32-C3-Boards (SuperMini) sind bei voller Leistung oft unsichtbar &ndash; dort 8,5&ndash;15 dBm ausprobieren und auf Signal und Paketverlust achten.</div>
      <div class="card-foot"><span class="hint">Ohne WLAN-Verbindung startet der ESP einen eigenen Access-Point (192.168.4.1). F&auml;llt das WLAN l&auml;nger als 60&nbsp;s aus, wird er ebenfalls aktiviert.</span></div>
    </div>
  </div>
</section>

<!-- ========== Reiter: VPN (WireGuard) ========== -->
<section id="t-vp" class="hide">
  <div class="layout">
    <div class="card span-6">
      <div class="card-head"><h2>WireGuard</h2><span class="card-meta">Tunnel zu einem entfernten Netz</span></div>
      <label class="chk"><input type="checkbox" id="wgEnabled">WireGuard-Tunnel aktiv</label>
      <div class="lrow">
        <label>Konfiguration einf&uuml;gen (wg-quick-Format, z.&nbsp;B. aus der FRITZ!Box)</label>
        <label class="fbtn">Datei laden<input type="file" accept=".conf,.txt" onchange="loadWireguardFile(this)" hidden></label>
      </div>
      <textarea id="wgImport" class="secret masked" rows="4" spellcheck="false"
                placeholder="[Interface]&#10;PrivateKey = ...&#10;Address = 192.168.178.201/24&#10;[Peer]&#10;..."></textarea>
      <div class="card-foot" style="padding-top:8px;margin-top:8px">
        <button class="btn sec small" onclick="applyWireguardImport()">Felder aus Konfiguration &uuml;bernehmen</button>
      </div>
      <div class="subsection">
        <div class="row">
          <div><label>Tunnel-Adresse (Address)</label><input id="wgAddress" placeholder="192.168.178.201/24"></div>
          <div><label>Keepalive (s)</label><input id="wgKeepalive" type="number" min="0" max="65535"></div>
        </div>
        <label>Private Key (eigener Schl&uuml;ssel)</label>
        <input id="wgPrivateKey" type="password" autocomplete="off" spellcheck="false">
        <label>Public Key der Gegenstelle</label>
        <input id="wgPeerPublicKey" autocomplete="off" spellcheck="false">
        <label>Preshared Key (optional)</label>
        <input id="wgPresharedKey" type="password" autocomplete="off" spellcheck="false">
        <div class="row">
          <div><label>Endpunkt (Host)</label><input id="wgEndpoint" placeholder="xyz.myfritz.net"></div>
          <div><label>Endpunkt-Port</label><input id="wgPort" type="number" min="1" max="65535"></div>
        </div>
        <label>Netze &uuml;ber den Tunnel (AllowedIPs)</label>
        <input id="wgAllowedIps" placeholder="192.168.178.0/24">
      </div>
    </div>
    <div class="span-6 stack">
      <div class="card">
        <div class="card-head"><h2>Status</h2></div>
        <div class="kv" id="vpnstat"></div>
      </div>
      <div class="card">
        <div class="card-head"><h2>Einrichtung mit einer FRITZ!Box</h2></div>
        <div class="hint" style="margin-top:0">
          1. In der FRITZ!Box unter <b>Internet &rarr; Freigaben &rarr; VPN (WireGuard)</b> eine Verbindung f&uuml;r ein
          &bdquo;Einzelger&auml;t&ldquo; anlegen.<br>
          2. Die angebotene Konfigurationsdatei herunterladen und hier mit &bdquo;Datei laden&ldquo; einlesen.<br>
          3. Speichern &ndash; der ESP startet neu, stellt die Uhr per NTP und baut den Tunnel auf.<br>
          Nur die Netze aus AllowedIPs laufen durch den Tunnel (z.&nbsp;B. das GoodWe-Gateway); WLAN, MQTT und
          Weboberfl&auml;che bleiben im lokalen Netz. Ein Eintrag 0.0.0.0/0 wird bewusst ignoriert.
        </div>
      </div>
    </div>
  </div>
  <div class="actions">
    <span class="hint">Der Tunnel wird nach einem Neustart mit den neuen Einstellungen aufgebaut.</span>
    <button class="btn" onclick="saveVpn()">Speichern &amp; neu starten</button>
  </div>
</section>

<!-- ========== Reiter: System ========== -->
<section id="t-sy" class="hide">
  <div class="layout">
    <div class="card span-6">
      <div class="card-head"><h2>Firmware-Update</h2><span class="card-meta">OTA mit Rollback</span></div>
      <div class="kv" id="otastat"></div>
      <label>Firmware-Datei (firmware.bin)</label>
      <label class="file-pick">
        <input type="file" id="fw" accept=".bin" onchange="showChosenFileName(this, 'fwName')" hidden>
        <span class="btn sec small">Datei w&auml;hlen</span><span class="file-name" id="fwName">Keine Datei gew&auml;hlt</span>
      </label>
      <div id="otap" class="hint"></div>
      <div class="hint">Wird in die inaktive Partition geschrieben und gepr&uuml;ft (Chip-Typ, Projektname, SHA-256). Nach dem Neustart l&auml;uft sie 60&nbsp;s auf Bew&auml;hrung; st&uuml;rzt sie ab, startet automatisch wieder die alte Firmware.</div>
      <div class="card-foot">
        <button class="btn" onclick="uploadFirmware()">Hochladen &amp; installieren</button>
        <button class="btn sec" id="rbbtn" onclick="rollbackFirmware()">Vorherige Firmware</button>
      </div>
    </div>
    <div class="card span-6">
      <div class="card-head"><h2>Web-Login</h2></div>
      <label class="chk"><input type="checkbox" id="webAuth">Login aktiv (Benutzer/Passwort abfragen)</label>
      <label>Benutzer</label>
      <input id="webUser" autocomplete="username">
      <div class="row">
        <div><label>Passwort</label><input id="webPass" type="password" placeholder="min. 4 Zeichen" autocomplete="new-password"></div>
        <div><label>Passwort wiederholen</label><input id="webPass2" type="password" autocomplete="new-password"></div>
      </div>
      <div class="card-foot"><button class="btn" onclick="saveLogin()">Login speichern</button></div>
    </div>
    <div class="card span-4">
      <div class="card-head"><h2>Uhrzeit</h2><span class="card-meta">NTP</span></div>
      <div class="kv" id="timestat"></div>
      <div class="row">
        <div><label>NTP-Server 1</label><input id="ntpServer1"></div>
        <div><label>NTP-Server 2</label><input id="ntpServer2"></div>
      </div>
      <label>Zeitzone (POSIX)</label>
      <input id="timeZone" list="timezones">
      <datalist id="timezones">
        <option value="CET-1CEST,M3.5.0,M10.5.0/3">Mitteleuropa (Berlin, Wien, Z&uuml;rich)</option>
        <option value="GMT0BST,M3.5.0/1,M10.5.0">Gro&szlig;britannien</option>
        <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Osteuropa</option>
        <option value="UTC0">UTC</option>
      </datalist>
      <div class="card-foot"><button class="btn" onclick="saveTime()">Speichern &amp; neu starten</button></div>
    </div>
    <div class="card span-4">
      <div class="card-head"><h2>Einstellungen sichern</h2></div>
      <div class="hint">Die Sicherung enth&auml;lt alle Einstellungen inklusive Passw&ouml;rtern und Zertifikaten im Klartext.</div>
      <label>Sicherung wiederherstellen</label>
      <label class="file-pick">
        <input type="file" id="restore" accept=".json" onchange="showChosenFileName(this, 'restoreName')" hidden>
        <span class="btn sec small">Datei w&auml;hlen</span><span class="file-name" id="restoreName">Keine Datei gew&auml;hlt</span>
      </label>
      <div class="card-foot">
        <a class="btn sec" href="/api/config/backup">Sicherung herunterladen</a>
        <button class="btn" onclick="restoreBackup()">Wiederherstellen</button>
      </div>
    </div>
    <div class="card span-4">
      <div class="card-head"><h2>System</h2></div>
      <div class="kv" id="sys"></div>
      <div class="card-foot">
        <button class="btn sec" onclick="rebootDevice()">Neustart</button>
        <button class="btn danger" onclick="factoryReset()">Werkseinstellungen</button>
      </div>
    </div>
  </div>
</section>

</main>

<div id="toast"></div>

<script>
// =====================================================================
// Hilfsfunktionen
// =====================================================================

/**
 * Kurzform für document.getElementById.
 * @param {string} id  Element-ID
 * @returns {HTMLElement|null} das Element oder null
 */
const byId = id => document.getElementById(id);

/** Zuletzt geladene Konfiguration (Antwort von /api/config). */
let config = null;

/** Anzeigenamen der Port-Rollen. */
/** Funktionen, die ein RS485-Port haben kann (Auswahl im Reiter Modbus). */
const ROLE_LABELS = {
  off: 'Aus',
  goodwe: 'GoodWe auslesen (Master)',
  lumel: 'Lumel N43 simulieren (Slave)'
};

/**
 * Blendet unten am Bildschirmrand eine kurze Meldung für 3,5 s ein.
 * Ein laufender Timer wird neu gestartet, damit schnelle Folgemeldungen sichtbar bleiben.
 * @param {string} text  anzuzeigender Text
 */
function showToast(text) {
  const toastElement = byId('toast');
  toastElement.textContent = text;
  toastElement.classList.add('on');
  clearTimeout(toastElement._t);
  toastElement._t = setTimeout(() => toastElement.classList.remove('on'), 3500);
}

/**
 * Maskiert &, <, > und " für die sichere Ausgabe in innerHTML
 * (Werte wie SSID oder Fehlertexte stammen von außen).
 * @param {*} value  beliebiger Wert, wird in einen String umgewandelt
 * @returns {string} HTML-sicherer Text
 */
function escapeHtml(value) {
  return String(value).replace(/[&<>"]/g, character => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;'
  }[character]));
}

/**
 * Schreibt ein Objekt als Schlüssel/Wert-Liste (Grid mit Klasse .kv) in ein Element.
 * Werte werden NICHT maskiert, damit sie HTML-Hervorhebungen enthalten dürfen.
 * @param {string} elementId  ID des Zielelements
 * @param {Object<string, string|number>} entries  Beschriftung -> Wert (HTML)
 */
function renderKeyValueList(elementId, entries) {
  byId(elementId).innerHTML = Object.entries(entries)
    .map(([key, value]) => `<span>${key}</span><span>${value}</span>`)
    .join('');
}

/**
 * Formatiert eine Zahl mit fester Anzahl Nachkommastellen.
 * @param {number|null} value  Zahl (null/NaN wird als '–' angezeigt)
 * @param {number} [decimals=1]  Anzahl Nachkommastellen
 * @returns {string} formatierter Wert
 */
function formatNumber(value, decimals = 1) {
  return value == null || isNaN(value) ? '–' : Number(value).toFixed(decimals);
}

/**
 * Wandelt ein Alter in Sekunden in lesbaren Text um.
 * @param {number} seconds  Alter in Sekunden (negativ = noch nie)
 * @returns {string} 'nie', 'xx s' (unter 2 min) oder 'xx min'
 */
function formatAge(seconds) {
  return seconds < 0 ? 'nie' : seconds < 120 ? seconds + ' s' : Math.round(seconds / 60) + ' min';
}

/**
 * Sendet JSON per POST an die Firmware und prüft das Feld "ok" der Antwort.
 * @param {string} url  API-Pfad, z. B. '/api/config'
 * @param {Object} [body]  zu sendende Daten (ohne Angabe wird '{}' gesendet)
 * @returns {Promise<Object>} die JSON-Antwort
 * @throws {Error} wenn die Antwort kein JSON ist oder ok == false meldet
 */
async function postJson(url, body) {
  const response = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : '{}'
  });
  const result = await response.json().catch(() => ({ ok: false, error: 'HTTP ' + response.status }));
  if (!result.ok) throw new Error(result.error || 'Fehler');
  return result;
}

// =====================================================================
// Reiter-Navigation
// =====================================================================

/** Reiter, die im Untermenü von "System" liegen. */
const SYSTEM_TABS = ['sy', 'co', 'mq', 'wl', 'vp', 'mb', 'gp'];
/** Zuletzt geöffneter System-Reiter (wird beim Klick auf "System" wieder geöffnet). */
let lastSystemTab = 'sy';

/**
 * Zeigt einen Reiter an: Abschnitt id="t-<tabId>" einblenden, Haupt- und Untermenü hervorheben
 * und den Reiter in der Adresszeile vermerken (#co usw.), damit Neuladen und Lesezeichen ihn behalten.
 * @param {string} tabId  Kürzel des Reiters, z. B. 'ov' oder 'co'
 */
function showTab(tabId) {
  const isSystemTab = SYSTEM_TABS.includes(tabId);
  if (isSystemTab) lastSystemTab = tabId;
  document.querySelectorAll('#nav button').forEach(button => button.classList.toggle('on',
    isSystemTab ? button.dataset.group == 'system' : button.dataset.t == tabId));
  document.querySelectorAll('#subnav button').forEach(button => button.classList.toggle('on', button.dataset.t == tabId));
  byId('subnav').classList.toggle('hide', !isSystemTab);
  document.querySelectorAll('main>section')
    .forEach(section => section.classList.toggle('hide', section.id != 't-' + tabId));
  history.replaceState(null, '', '#' + tabId);
  const activeButton = document.querySelector(`#subnav button[data-t="${tabId}"], #nav button[data-t="${tabId}"]`);
  if (activeButton) activeButton.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}

// Hauptleiste: "System" öffnet den zuletzt benutzten System-Reiter (über dessen Button, damit
// dort hinterlegte Aktionen wie das Verbinden der Konsole ausgeführt werden)
document.querySelectorAll('#nav button').forEach(button => button.addEventListener('click', () => {
  if (button.dataset.group == 'system') document.querySelector(`#subnav button[data-t="${lastSystemTab}"]`).click();
  else showTab(button.dataset.t);
}));
document.querySelectorAll('#subnav button').forEach(button => button.addEventListener('click', () => showTab(button.dataset.t)));

// =====================================================================
// Formulare für die Modbus-Ports und GPIOs (dynamisch erzeugt)
// =====================================================================

const BAUD_RATES = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200];
const PARITY_OPTIONS = [['N', 'keine'], ['E', 'gerade (even)'], ['O', 'ungerade (odd)']];
const STRAPPING_PINS = [4, 5, 8, 9, 15];

/**
 * Erzeugt die HTML-Karte mit den Schnittstellenparametern eines RTU-Ports
 * (Rolle, Baudrate, Adresse, Parität, Stopbits) für den Reiter Modbus.
 * @param {number} index  Port-Index (0 = RTU1, 1 = RTU2)
 * @param {Object} port  Port-Konfiguration aus /api/config
 * @returns {string} HTML der Karte
 */
/**
 * Erzeugt den Knopf "Identify" für RTU1/RTU2.
 * @param {number} index  0 = RTU1, 1 = RTU2
 * @returns {string} HTML
 */
function renderIdentifyButton(index) {
  return `<button class="btn sec small identify-btn" type="button" onclick="identifyPort(${index})"`
    + ` title="TXD-LED des RS485-Moduls 10 s blinken lassen">Identify</button>`;
}

/**
 * Lässt die TXD-LED des RS485-Moduls von RTU1/RTU2 10 s lang blinken, um das Modul zuzuordnen.
 * Läuft der Port, ist der Busverkehr währenddessen gestört.
 * @param {number} index  0 = RTU1, 1 = RTU2
 * @returns {Promise<void>}
 */
async function identifyPort(index) {
  try {
    const result = await (await fetch('/api/identify?port=' + (index + 1), { method: 'POST' })).json();
    showToast(result.ok ? `RTU${index + 1}: TXD-LED blinkt ${result.seconds} s` : `RTU${index + 1}: ${result.error}`);
  } catch (error) {
    showToast('Fehler: ' + error.message);
  }
}

function renderPortForm(index, port) {
  // Läuft der GoodWe über Modbus TCP, kann kein RS485-Port gleichzeitig die Rolle GoodWe haben
  const goodweViaTcp = config && config.gwTransport == 'tcp';
  const roleOptions = Object.entries(ROLE_LABELS)
    .map(([key, label]) => {
      const blocked = key == 'goodwe' && goodweViaTcp;
      return `<option value="${key}"${port.role == key ? ' selected' : ''}${blocked ? ' disabled' : ''}>`
        + `${label}${blocked ? ' – GoodWe läuft über Modbus TCP' : ''}</option>`;
    })
    .join('');
  const baudOptions = BAUD_RATES
    .map(baud => `<option${port.baud == baud ? ' selected' : ''}>${baud}</option>`)
    .join('');
  const parityOptions = PARITY_OPTIONS
    .map(([key, label]) => `<option value="${key}"${port.parity == key ? ' selected' : ''}>${label}</option>`)
    .join('');

  return `<div class="card span-6">
    <div class="card-head"><h2>RTU${index + 1}</h2><span class="card-meta">UART${index == 0 ? 1 : 0} ${renderIdentifyButton(index)}</span></div>
    <label>Funktion</label><select id="role${index}">${roleOptions}</select>
    <div class="row">
      <div><label>Baudrate</label><select id="baud${index}">${baudOptions}</select></div>
      <div><label id="al${index}">Adresse</label><input id="addr${index}" type="number" min="1" max="247" value="${port.addr}"></div>
    </div>
    <div class="row">
      <div><label>Parit&auml;t</label><select id="par${index}" onchange="updateParityHint(${index})">${parityOptions}</select></div>
      <div><label>Stopbits</label><select id="sb${index}" onchange="updateParityHint(${index})"><option${port.stopBits == 1 ? ' selected' : ''}>1</option><option${port.stopBits == 2 ? ' selected' : ''}>2</option></select></div>
    </div>
    <div class="hint" id="ph${index}"></div>
  </div>`;
}

/**
 * Zeigt unter dem Port-Formular an, ob die gewählte Kombination aus Parität und
 * Stopbits der Modbus-Spezifikation (11 Bit pro Zeichen) entspricht.
 * Wird beim Laden und bei jeder Änderung von Parität/Stopbits aufgerufen.
 * @param {number} index  Port-Index
 */
function updateParityHint(index) {
  const parity = byId('par' + index).value;
  const stopBits = +byId('sb' + index).value;
  let hint;
  if (parity == 'N' && stopBits == 1) {
    hint = '<span class="warn">Modbus-Spec: ohne Parit&auml;t sind 2 Stopbits vorgeschrieben (11 Bit/Zeichen). GoodWe verwendet trotzdem 8N1.</span>';
  } else if (parity != 'N' && stopBits == 2) {
    hint = '<span class="warn">Mit Parit&auml;t schreibt die Spec 1 Stopbit vor.</span>';
  } else {
    hint = 'Konform zu Modbus over Serial Line (11 Bit/Zeichen).';
  }
  byId('ph' + index).innerHTML = hint;
}

/**
 * Passt die Beschriftung des Adressfelds an die gewählte Rolle an:
 * als Master ist es die Adresse des GoodWe, als Slave die eigene Adresse.
 * @param {number} index  Port-Index
 */
function updateAddressLabel(index) {
  byId('al' + index).textContent = byId('role' + index).value == 'goodwe' ? 'Adresse des GoodWe' : 'Eigene Slave-Adresse';
}

/**
 * Erzeugt eine Auswahlliste der von der Firmware erlaubten GPIOs;
 * Strapping-Pins werden gekennzeichnet.
 * @param {string} selectId  ID des select-Elements
 * @param {number} selectedGpio  aktuell zugeordneter GPIO (-1 = Auto)
 * @param {boolean} [allowAuto]  true: Option "Auto (kein DE-Pin)" anbieten (nur DE/RE)
 * @returns {string} HTML des select-Elements
 */
function renderGpioSelect(selectId, selectedGpio, allowAuto) {
  const autoOption = allowAuto
    ? `<option value="-1"${selectedGpio == -1 ? ' selected' : ''}>Auto (kein DE-Pin)</option>`
    : '';
  const gpioOptions = config.validGpios
    .map(gpio => `<option value="${gpio}"${gpio == selectedGpio ? ' selected' : ''}>GPIO ${gpio}${STRAPPING_PINS.includes(gpio) ? ' (Strapping)' : ''}</option>`)
    .join('');
  return `<select id="${selectId}">${autoOption}${gpioOptions}</select>`;
}

/**
 * Erzeugt die HTML-Karte mit der GPIO-Zuordnung (RX, TX, DE/RE) eines RTU-Ports
 * für den Reiter Settings / GPIO.
 * @param {number} index  Port-Index
 * @param {Object} port  Port-Konfiguration aus /api/config
 * @returns {string} HTML der Karte
 */
function renderGpioForm(index, port) {
  return `<div class="card span-6">
    <div class="card-head"><h2>RTU${index + 1}</h2><span class="card-meta">${ROLE_LABELS[port.role]} ${renderIdentifyButton(index)}</span></div>
    <div class="row3">
      <div><label>RX (RO)</label>${renderGpioSelect('rx' + index, port.rx)}</div>
      <div><label>TX (DI)</label>${renderGpioSelect('tx' + index, port.tx)}</div>
      <div><label>DE/RE</label>${renderGpioSelect('de' + index, port.de, true)}</div>
    </div>
    <div class="hint">RX = Ausgang RO des Transceivers, TX = Eingang DI, DE/RE = Richtungsumschaltung (DE und /RE br&uuml;cken).</div>
  </div>`;
}

// =====================================================================
// Passwortfelder: Auge-Symbol zum Anzeigen/Verbergen
// =====================================================================

const ICON_EYE = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-7 11-7 11 7 11 7-4 7-11 7S1 12 1 12z"/><circle cx="12" cy="12" r="3"/></svg>';
const ICON_EYE_OFF = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 19c-7 0-11-7-11-7a18.45 18.45 0 0 1 5.06-5.94"/><path d="M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 7 11 7a18.5 18.5 0 0 1-2.16 3.19"/><path d="M14.12 14.12a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>';

/**
 * Versieht jedes Passwortfeld und jedes geheime Textfeld (textarea.secret) mit einem
 * Auge-Button zum Anzeigen/Verbergen. Bereits umhüllte Felder werden übersprungen.
 * Textfelder werden per CSS-Klasse .masked verdeckt, Inputs über type="password".
 */
function addPasswordToggles() {
  document.querySelectorAll('input[type=password],textarea.secret').forEach(field => {
    if (field.parentNode.classList.contains('pw')) return;

    // Feld in einen Wrapper verschieben, damit der Button darüber positioniert werden kann
    const wrapper = document.createElement('div');
    wrapper.className = 'pw';
    field.parentNode.insertBefore(wrapper, field);
    wrapper.appendChild(field);

    const isTextarea = field.tagName == 'TEXTAREA';
    const isHidden = () => isTextarea ? field.classList.contains('masked') : field.type == 'password';

    const toggleButton = document.createElement('button');
    toggleButton.type = 'button';
    toggleButton.className = 'eye' + (isTextarea ? ' ta' : '');
    toggleButton.title = 'Anzeigen';
    toggleButton.setAttribute('aria-label', 'Anzeigen');
    toggleButton.innerHTML = ICON_EYE;

    toggleButton.onclick = () => {
      const show = isHidden();
      if (isTextarea) field.classList.toggle('masked', !show);
      else field.type = show ? 'text' : 'password';
      toggleButton.innerHTML = show ? ICON_EYE_OFF : ICON_EYE;
      toggleButton.title = show ? 'Verbergen' : 'Anzeigen';
      toggleButton.setAttribute('aria-label', toggleButton.title);
    };
    wrapper.appendChild(toggleButton);
  });
}

// =====================================================================
// MQTT-TLS
// =====================================================================

/**
 * TLS-Bereich und CA-Feld ein-/ausblenden.
 * portToggled = true: Port beim Umschalten von TLS zwischen 1883 und 8883 wechseln.
 */
/**
 * Blendet den TLS-Bereich und das CA-Zertifikatsfeld passend zur Auswahl ein/aus.
 * @param {boolean} portToggled  true, wenn TLS gerade umgeschaltet wurde: dann den Port
 *   zwischen den Standardwerten 1883 und 8883 wechseln (eigene Ports bleiben unverändert)
 */
function updateTlsVisibility(portToggled) {
  const tlsEnabled = byId('mqttTls').checked;
  byId('tlsbox').classList.toggle('hide', !tlsEnabled);
  byId('cabox').classList.toggle('hide', byId('mqttTlsMode').value != 'ca');

  if (portToggled) {
    const portInput = byId('mqttPort');
    if (tlsEnabled && portInput.value == '1883') portInput.value = 8883;
    if (!tlsEnabled && portInput.value == '8883') portInput.value = 1883;
  }
}

/**
 * Liest eine ausgewählte PEM-Datei und schreibt ihren Inhalt in ein Textfeld.
 * Das Dateifeld wird danach geleert, damit dieselbe Datei erneut gewählt werden kann.
 * @param {HTMLInputElement} fileInput  Dateiauswahl-Element
 * @param {string} targetId  ID des Ziel-Textfelds
 */
function loadPemFile(fileInput, targetId) {
  const file = fileInput.files[0];
  if (!file) return;
  file.text().then(text => {
    byId(targetId).value = text.trim();
    showToast('Datei geladen: ' + file.name);
  });
  fileInput.value = '';
}

addPasswordToggles();

// =====================================================================
// Konfiguration laden und in die Formulare übernehmen
// =====================================================================

const SECRET_FIELDS = ['wifiPass', 'apPass', 'mqttPass', 'webPass', 'mqttCa', 'mqttCert', 'mqttKey',
  'wgPrivateKey', 'wgPresharedKey'];
const VALUE_FIELDS = ['wgAddress', 'wgKeepalive', 'wgPeerPublicKey', 'wgEndpoint', 'wgPort', 'wgAllowedIps',
  'ntpServer1', 'ntpServer2', 'timeZone', 'gwSource', 'gwTransport', 'gwTcpHost', 'gwTcpPort', 'gwTcpUnit', 'bridgePort', 'gwPollMs', 'gwSlowPollMs', 'gwTimeoutMs', 'staleSec', 'testPowerW', 'testVoltage',
  'wifiSsid', 'wifiTxPower', 'hostname', 'webUser', 'mqttHost', 'mqttPort', 'mqttUser', 'mqttBase',
  'mqttIntervalSec', 'mqttDiscPrefix'];
const CHECKBOX_FIELDS = ['wgEnabled', 'bridgeEnabled', 'gwInvertSign', 'lumelWordSwap', 'lumelSilentOnStale', 'lumelNoUndefined', 'testMode', 'webAuth',
  'mqttEnabled', 'mqttSingleTopics', 'mqttDiscovery'];

/**
 * Lädt die Konfiguration von /api/config, speichert sie in `config` und füllt
 * alle Formularfelder; die Port- und GPIO-Formulare werden dabei neu erzeugt.
 * @returns {Promise<void>}
 */
async function loadConfig() {
  config = await (await fetch('/api/config')).json();

  SECRET_FIELDS.forEach(key => byId(key).value = config[key] || '');
  byId('mqttTls').checked = config.mqttTls;
  byId('mqttTlsMode').value = config.mqttTlsMode || 'ca';
  updateTlsVisibility(false);
  byId('webPass2').value = config.webPass || '';

  // Port-Formulare (Reiter Modbus)
  byId('portcfg').innerHTML = config.ports.map((port, index) => renderPortForm(index, port)).join('');
  config.ports.forEach((port, index) => {
    updateParityHint(index);
    byId('role' + index).onchange = () => updateAddressLabel(index);
    updateAddressLabel(index);
  });

  // GPIO-Formulare (Reiter Settings / GPIO)
  byId('gpiocfg').innerHTML = config.ports.map((port, index) => renderGpioForm(index, port)).join('');

  VALUE_FIELDS.forEach(key => byId(key).value = config[key]);
  updateTransportVisibility();
  CHECKBOX_FIELDS.forEach(key => byId(key).checked = config[key]);
}

// =====================================================================
// Speichern
// =====================================================================

/**
 * Sendet eine (Teil-)Konfiguration an /api/config und meldet das Ergebnis.
 * Verlangt die Firmware einen Neustart, wird die Seite nach 8 s neu geladen,
 * sonst wird die Konfiguration neu eingelesen.
 * @param {Object} body  zu speichernde Felder
 * @param {string} [successMessage]  Meldung bei Erfolg ohne Neustart
 * @returns {Promise<void>}
 */
async function saveConfig(body, successMessage) {
  try {
    const result = await postJson('/api/config', body);
    showToast(result.reboot ? 'Gespeichert – Neustart ...' : (successMessage || 'Gespeichert'));
    if (result.reboot) setTimeout(() => location.reload(), 8000);
    else loadConfig();
  } catch (error) {
    showToast('Fehler: ' + error.message);
  }
}

/**
 * Speichert Port-Parameter, GoodWe-, Lumel- und Testmodus-Einstellungen (Reiter Modbus).
 */
function saveModbus() {
  const viaTcp = byId('gwTransport').value == 'tcp';
  saveConfig({
    ports: [0, 1].map(index => ({
      // bei Anbindung über TCP wird ein RTU-Port mit Rolle GoodWe automatisch abgeschaltet
      role: viaTcp && byId('role' + index).value == 'goodwe' ? 'off' : byId('role' + index).value,
      baud: +byId('baud' + index).value,
      parity: byId('par' + index).value,
      stopBits: +byId('sb' + index).value,
      addr: +byId('addr' + index).value
    })),
    gwSource: byId('gwSource').value,
    gwTransport: byId('gwTransport').value,
    gwTcpHost: byId('gwTcpHost').value.trim(),
    gwTcpPort: +byId('gwTcpPort').value,
    gwTcpUnit: +byId('gwTcpUnit').value,
    bridgeEnabled: byId('bridgeEnabled').checked,
    bridgePort: +byId('bridgePort').value,
    gwPollMs: +byId('gwPollMs').value,
    gwSlowPollMs: +byId('gwSlowPollMs').value,
    gwTimeoutMs: +byId('gwTimeoutMs').value,
    gwInvertSign: byId('gwInvertSign').checked,
    lumelWordSwap: byId('lumelWordSwap').checked,
    lumelSilentOnStale: byId('lumelSilentOnStale').checked,
    lumelNoUndefined: byId('lumelNoUndefined').checked,
    staleSec: +byId('staleSec').value,
    testMode: byId('testMode').checked,
    testPowerW: +byId('testPowerW').value,
    testVoltage: +byId('testVoltage').value
  });
}

/**
 * Speichert die GPIO-Zuordnung beider Ports (Reiter Settings / GPIO).
 * Bricht mit einer Meldung ab, wenn ein GPIO bei aktiven Ports doppelt belegt ist.
 */
function saveGpio() {
  const ports = [0, 1].map(index => ({
    rx: +byId('rx' + index).value,
    tx: +byId('tx' + index).value,
    de: +byId('de' + index).value
  }));

  // Doppelbelegung nur bei aktiven Ports prüfen (DE = -1 bedeutet "Auto")
  const activePorts = [0, 1].filter(index => config.ports[index].role != 'off');
  const usedGpios = activePorts
    .flatMap(index => [ports[index].rx, ports[index].tx, ports[index].de])
    .filter(gpio => gpio >= 0);
  if (new Set(usedGpios).size != usedGpios.length) return showToast('Ein GPIO ist doppelt belegt');

  saveConfig({ ports });
}

/** Übernimmt die maximale WLAN-Sendeleistung sofort (ohne Neustart). */
function saveWifiTxPower() {
  saveConfig({ wifiTxPower: +byId('wifiTxPower').value }, 'Sendeleistung übernommen');
}

/**
 * Speichert WLAN-Zugangsdaten, Hostname und Access-Point-Passwort.
 * Ein leeres WLAN-Passwort wird über wifiPassClear ausdrücklich gelöscht.
 */
function saveWifi() {
  saveConfig({
    wifiSsid: byId('wifiSsid').value,
    wifiPass: byId('wifiPass').value,
    wifiPassClear: !byId('wifiPass').value,
    hostname: byId('hostname').value,
    apPass: byId('apPass').value
  });
}

/**
 * Speichert die Web-Login-Einstellungen, sofern beide Passworteingaben übereinstimmen.
 */
function saveLogin() {
  if (byId('webPass').value != byId('webPass2').value) return showToast('Passwörter stimmen nicht überein');
  saveConfig({
    webAuth: byId('webAuth').checked,
    webUser: byId('webUser').value,
    webPass: byId('webPass').value
  }, byId('webAuth').checked ? 'Login aktiv – ggf. neu anmelden' : 'Login deaktiviert');
}

/**
 * Speichert alle MQTT-Einstellungen inklusive TLS und Home-Assistant-Discovery.
 * Ein leeres Passwort wird über mqttPassClear ausdrücklich gelöscht.
 */
function saveMqtt() {
  saveConfig({
    mqttEnabled: byId('mqttEnabled').checked,
    mqttHost: byId('mqttHost').value,
    mqttPort: +byId('mqttPort').value,
    mqttUser: byId('mqttUser').value,
    mqttPass: byId('mqttPass').value,
    mqttPassClear: !byId('mqttPass').value,
    mqttBase: byId('mqttBase').value,
    mqttIntervalSec: +byId('mqttIntervalSec').value,
    mqttSingleTopics: byId('mqttSingleTopics').checked,
    mqttDiscovery: byId('mqttDiscovery').checked,
    mqttDiscPrefix: byId('mqttDiscPrefix').value,
    mqttTls: byId('mqttTls').checked,
    mqttTlsMode: byId('mqttTlsMode').value,
    mqttCa: byId('mqttCa').value,
    mqttCert: byId('mqttCert').value,
    mqttKey: byId('mqttKey').value
  }, 'MQTT gespeichert');
}

// =====================================================================
// Aktionen (WLAN-Suche, MQTT-Discovery, Neustart, Werksreset)
// =====================================================================

/**
 * Sucht WLAN-Netze über /api/scan, füllt die SSID-Vorschlagsliste und zeigt
 * die ersten acht gefundenen Netze als Hinweis an.
 * @returns {Promise<void>}
 */
async function scanWifiNetworks() {
  byId('scanres').textContent = 'Suche ...';
  try {
    const result = await (await fetch('/api/scan')).json();
    byId('ssids').innerHTML = result.networks
      .map(network => `<option value="${escapeHtml(network.ssid)}">${network.rssi} dBm</option>`)
      .join('');
    byId('scanres').textContent = result.networks.length + ' Netze gefunden: '
      + result.networks.slice(0, 8).map(network => network.ssid + ' (' + network.rssi + ')').join(', ');
  } catch (error) {
    byId('scanres').textContent = 'Suche fehlgeschlagen';
  }
}

/**
 * Fordert die Firmware auf, die Home-Assistant-Discovery-Nachrichten erneut zu senden.
 */
function resendDiscovery() {
  postJson('/api/mqtt/discovery').then(() => showToast('Discovery wird neu gesendet'));
}

/**
 * Startet den ESP32 neu.
 */
function rebootDevice() {
  postJson('/api/reboot').then(() => showToast('Neustart ...'));
}

/**
 * Setzt nach Rückfrage alle Einstellungen auf Werkszustand zurück (inkl. WLAN und Login).
 */
function factoryReset() {
  if (confirm('Alle Einstellungen (inkl. WLAN und Login) zurücksetzen?')) {
    postJson('/api/factory').then(() => showToast('Zurückgesetzt – Neustart ...'));
  }
}

// =====================================================================
// Firmware-Update (OTA), Rollback, Sicherung wiederherstellen
// =====================================================================

/**
 * Lädt die gewählte firmware.bin per OTA hoch. Verwendet XMLHttpRequest statt fetch,
 * weil nur so der Upload-Fortschritt angezeigt werden kann. Bei Erfolg wird die
 * Seite nach 12 s neu geladen.
 */
function uploadFirmware() {
  const firmwareFile = byId('fw').files[0];
  if (!firmwareFile) return showToast('Bitte firmware.bin wählen');

  const request = new XMLHttpRequest();
  request.open('POST', '/api/update');
  request.setRequestHeader('Content-Type', 'application/octet-stream');

  request.upload.onprogress = event =>
    byId('otap').textContent = 'Übertragen: ' + Math.round(event.loaded / event.total * 100) + ' %';

  request.onload = () => {
    let errorMessage = '';
    try { errorMessage = JSON.parse(request.responseText).error; } catch (error) {}
    byId('otap').innerHTML = request.status == 200
      ? '<span class="ok">Installiert – Neustart. Die neue Firmware wird nach 60 s best&auml;tigt.</span>'
      : '<span class="bad">Fehler: ' + escapeHtml(errorMessage || request.responseText) + '</span>';
    if (request.status == 200) setTimeout(() => location.reload(), 12000);
  };

  request.onerror = () => byId('otap').innerHTML = '<span class="bad">Verbindung abgebrochen</span>';
  request.send(firmwareFile);
}

/**
 * Schaltet nach Rückfrage auf die Firmware der anderen OTA-Partition zurück
 * und lädt die Seite nach 10 s neu.
 */
function rollbackFirmware() {
  if (!confirm('Auf die Firmware der anderen Partition zurückschalten und neu starten?')) return;
  postJson('/api/ota/rollback')
    .then(() => {
      showToast('Umschalten – Neustart ...');
      setTimeout(() => location.reload(), 10000);
    })
    .catch(error => showToast('Fehler: ' + error.message));
}

/**
 * Liest eine Sicherungsdatei (JSON) ein und übernimmt sie nach Rückfrage als Konfiguration.
 * @returns {Promise<void>}
 */
async function restoreBackup() {
  const backupFile = byId('restore').files[0];
  if (!backupFile) return showToast('Bitte Sicherungsdatei wählen');
  try {
    const backup = JSON.parse(await backupFile.text());
    if (!confirm('Alle Einstellungen aus der Sicherung übernehmen?')) return;
    saveConfig(backup, 'Wiederhergestellt');
  } catch (error) {
    showToast('Ungültige Datei');
  }
}

// =====================================================================
// Reiter GoodWe: alle Werte (nur laden, wenn der Reiter sichtbar ist)
// =====================================================================

/** Geräte des GoodWe (Reihenfolge = GoodweDevice in goodwe_sensors.h); je Gerät eine Karte. */
const GOODWE_DEVICES = [
  { key: 'info', name: 'Info' },
  { key: 'inverter', name: 'Wechselrichter' },
  { key: 'meter', name: 'Smart-Meter' },
  { key: 'battery1', name: 'Batterie 1' },
  { key: 'battery2', name: 'Batterie 2' }
];
/** Gerät "Info" (nur Nennleistung) – keine eigene Karte, sondern in der Kopfzeile. */
const DEVICE_INFO = 0;

/**
 * Erzeugt den Umschalter Intervall 1/2 für einen Wert. Von der Lumel-Emulation benötigte
 * Werte sind fest Intervall 1 zugeordnet.
 * @param {string} sensorId  MQTT-/Sensor-ID
 * @param {number} pollGroup  1 = Intervall 1, 2 = Intervall 2
 * @param {boolean} requiredForLumel
 * @returns {string} HTML
 */
function renderPollToggle(sensorId, pollGroup, requiredForLumel) {
  if (requiredForLumel)
    return '<span class="poll-toggle locked" title="Wird f&uuml;r die Lumel-Emulation immer in Intervall 1 gelesen">'
      + '<button class="on" disabled>1</button><button disabled>2</button></span>';
  return `<span class="poll-toggle">`
    + `<button class="${pollGroup == 1 ? 'on' : ''}" onclick="setPollGroup('${sensorId}', 1)" title="Intervall 1 (schnell)">1</button>`
    + `<button class="${pollGroup == 2 ? 'on' : ''}" onclick="setPollGroup('${sensorId}', 2)" title="Intervall 2 (langsam)">2</button></span>`;
}

/**
 * Ordnet einen Wert Intervall 1 oder 2 zu und speichert die Auswahl sofort (ohne Neustart).
 * @param {string} sensorId
 * @param {number} pollGroup  1 oder 2
 * @returns {Promise<void>}
 */
async function setPollGroup(sensorId, pollGroup) {
  const fastSensorIds = new Set(config.gwFast || []);
  if (pollGroup == 1) fastSensorIds.add(sensorId);
  else fastSensorIds.delete(sensorId);
  config.gwFast = [...fastSensorIds];
  try {
    await postJson('/api/config', { gwFast: config.gwFast });
    showToast(`${sensorId}: Intervall ${pollGroup}`);
    loadGoodweValues();
  } catch (error) {
    showToast('Fehler: ' + error.message);
  }
}

/**
 * Schaltet ein Gerät des GoodWe (Smart-Meter, Batterie 1/2) ein oder aus und speichert sofort.
 * Abgeschaltete Geräte werden nicht abgefragt und überall ausgeblendet.
 * @param {string} deviceKey  z. B. 'battery1'
 * @param {boolean} enabled
 * @returns {Promise<void>}
 */
async function setDeviceEnabled(deviceKey, enabled) {
  const disabledDevices = new Set(config.gwDisabled || []);
  if (enabled) disabledDevices.delete(deviceKey);
  else disabledDevices.add(deviceKey);
  try {
    await postJson('/api/config', { gwDisabled: [...disabledDevices] });
    config.gwDisabled = [...disabledDevices];
    const deviceName = GOODWE_DEVICES.find(device => device.key == deviceKey).name;
    showToast(`${deviceName} ${enabled ? 'aktiviert' : 'abgeschaltet'}`);
    loadGoodweValues();
  } catch (error) {
    showToast('Fehler: ' + error.message);
    loadGoodweValues();
  }
}

/**
 * Lädt alle GoodWe-Werte von /api/goodwe und stellt sie als Tabelle dar, gruppiert
 * nach Registerblock. Läuft nur, wenn der Reiter GoodWe sichtbar ist, um den ESP
 * nicht unnötig zu belasten. Fehler werden still ignoriert (nächster Versuch in 2 s).
 * @returns {Promise<void>}
 */
async function loadGoodweValues() {
  if (byId('t-gw').classList.contains('hide')) return;
  try {
    const goodwe = await (await fetch('/api/goodwe')).json();

    byId('gwtitle').textContent = 'Alle Werte – ' + deviceDisplayName(goodwe.model);
    byId('gwinfo').innerHTML =
      `Seriennr. ${escapeHtml(goodwe.serial || '–')} · Datenalter ${goodwe.age < 0 ? '–' : goodwe.age + ' s'} · Bl&ouml;cke: `
      + Object.entries(goodwe.blocks)
        .map(([blockName, blockOk]) => `<span class="${blockOk ? 'ok' : 'bad'}">${blockName}</span>`)
        .join(' ');

    // Sensoren: [id, name, block, register, value, unit, gruppe, lumel, gerät] -> nach Gerät gruppieren
    const rowsByDevice = {};
    let ratedPowerText = '';
    for (const [id, name, block, register, value, unit, pollGroup, requiredForLumel, device] of goodwe.sensors) {
      if (device == DEVICE_INFO) {
        if (id == 'rated_power') ratedPowerText = ` · Nennleistung ${escapeHtml(value)} ${escapeHtml(unit)}`;
        continue;
      }
      (rowsByDevice[device] = rowsByDevice[device] || []).push(
        `<tr title="MQTT-ID: ${id}"><td>${escapeHtml(name)}</td>`
        + `<td>${escapeHtml(value)} ${escapeHtml(unit)}</td><td>${register}</td>`
        + `<td>${renderPollToggle(id, pollGroup, requiredForLumel)}</td></tr>`);
    }
    byId('gwinfo').innerHTML += ratedPowerText;
    // Welche Register Intervall 1 tatsächlich liest (Lücken werden mitgelesen, aber verworfen)
    const fastRangeText = (goodwe.fastRanges || [])
      .map(([start, count]) => count > 1 ? `${start}–${start + count - 1}` : `${start}`).join(', ');
    byId('gwPollInfo').innerHTML = `<b>Intervall 1</b> (alle ${config.gwPollMs} ms) liest: ${fastRangeText || '–'}`
      + ` · <b>Intervall 2</b> (alle ${config.gwSlowPollMs} ms) liest alle Bl&ouml;cke vollst&auml;ndig.`
      + ` Mitgelesene L&uuml;cken spart Anfragen; &uuml;bernommen werden davon nur die Werte aus Intervall 1.`;
    const disabledDevices = config.gwDisabled || [];
    byId('gwBlocks').innerHTML = GOODWE_DEVICES.map((device, deviceIndex) => {
      if (deviceIndex == DEVICE_INFO) return '';
      const enabled = !disabledDevices.includes(device.key);
      const rows = rowsByDevice[deviceIndex] || [];
      // Das Gerät der Datenquelle (System › Modbus) liefert die Lumel-Werte und bleibt immer aktiv
      const locked = (device.key == 'meter' && config.gwSource == 'meter')
        || (device.key == 'inverter' && config.gwSource == 'inverter');
      const lockReason = `${device.name} ist die Datenquelle (System › Modbus)`;
      const toggle = `<label class="chk card-switch" title="${locked ? lockReason : 'Abfragen und überall anzeigen'}">`
        + `<input type="checkbox" ${enabled ? 'checked' : ''} ${locked ? 'disabled' : ''} `
        + `onchange="setDeviceEnabled('${device.key}', this.checked)">aktiv</label>`;
      const body = enabled && !rows.length
        ? '<div class="hint">Noch keine Werte &ndash; keine Verbindung oder vom Ger&auml;t nicht unterst&uuml;tzt.</div>'
        : enabled
        ? `<div class="table-wrap"><table class="value-table">
            <thead><tr><th>Wert</th><th>Inhalt</th><th>Register</th><th title="Abfrageintervall">Intervall</th></tr></thead>
            <tbody>${rows.join('')}</tbody>
          </table></div>`
        : '<div class="hint">Abgeschaltet &ndash; wird nicht abgefragt und nirgends angezeigt.</div>';
      return `<div class="card span-6${enabled ? '' : ' card-off'}">
        <div class="card-head"><h2>${device.name}</h2><span class="card-meta">${enabled ? rows.length + ' Werte · ' : ''}${toggle}</span></div>
        ${body}
      </div>`;
    }).join('');
  } catch (error) {}
}

// =====================================================================
// Reiter Lumel: Register und Werte, die die Simulation ausgibt
// =====================================================================

/** Messwerte 7500 + n des N43: [Bezeichnung, Einheit, Nachkommastellen] (docs/lumel_n43_register.md) */
const LUMEL_VALUE_NAMES = (() => {
  const names = [];
  for (const phase of ['L1', 'L2', 'L3'])
    names.push([`Spannung ${phase}`, 'V', 1], [`Strom ${phase}`, 'A', 3], [`Wirkleistung ${phase}`, 'W', 1],
      [`Blindleistung ${phase}`, 'var', 1], [`Scheinleistung ${phase}`, 'VA', 1], [`Leistungsfaktor ${phase}`, '', 3],
      [`tg φ ${phase}`, '', 3], [`THD U ${phase}`, '%', 1], [`THD I ${phase}`, '%', 1]);
  names.push(['Spannung Mittelwert', 'V', 1], ['Strom Mittelwert', 'A', 3], ['Wirkleistung gesamt', 'W', 1],
    ['Blindleistung gesamt', 'var', 1], ['Scheinleistung gesamt', 'VA', 1], ['Leistungsfaktor gesamt', '', 3],
    ['tg φ gesamt', '', 3], ['Frequenz', 'Hz', 2], ['Leiterspannung L1-L2', 'V', 1], ['Leiterspannung L2-L3', 'V', 1],
    ['Leiterspannung L3-L1', 'V', 1], ['Leiterspannung Mittelwert', 'V', 1], ['P Demand', 'W', 1], ['S Demand', 'VA', 1],
    ['I Demand', 'A', 3], ['THD U Mittelwert', '%', 1], ['THD I Mittelwert', '%', 1], ['Neutralleiterstrom', 'A', 3],
    ['Bezug – Überläufe', '× 100 MWh', 0], ['Bezug – Zähler', 'kWh', 1], ['Lieferung – Überläufe', '× 100 MWh', 0],
    ['Lieferung – Zähler', 'kWh', 1], ['Blindenergie ind. – Überläufe', '× 100 Mvarh', 0], ['Blindenergie ind. – Zähler', 'kvarh', 1],
    ['Blindenergie kap. – Überläufe', '× 100 Mvarh', 0], ['Blindenergie kap. – Zähler', 'kvarh', 1],
    ['Scheinenergie – Überläufe', '× 100 MVAh', 0], ['Scheinenergie – Zähler', 'kVAh', 1], ['Uhrzeit Sekunden', 's', 0],
    ['Uhrzeit Stunden,Minuten', '', 2], ['Monat,Tag', '', 2], ['Jahr', '', 0], ['Strom Mittelwert max', 'A', 3],
    ['Spannung max', 'V', 1], ['P Demand min', 'W', 1], ['P Demand max', 'W', 1], ['S Demand max', 'VA', 1],
    ['I Demand max', 'A', 3]);
  return names;
})();

/** Belegte Konfigurationsregister 4000 ff.: Register -> Bezeichnung */
const LUMEL_CONFIG_NAMES = {
  4003: 'Anschluss (0 = 3Ph/4W)', 4004: 'Stromeingang (1 = 5 A)', 4005: 'Stromwandler-Übersetzung',
  4006: 'Spannungswandler × 10', 4008: 'Synchronisation mit Uhr', 4038: 'Impulse je kWh', 4039: 'Modbus-Adresse',
  4040: 'Format (0 = 8N2, 1 = 8E1, 2 = 8O1, 3 = 8N1)', 4041: 'Baudrate (0 = 4800 … 3 = 38400)', 4045: 'Uhrzeit hhmm',
  4048: 'Bezug High-Word (100 Wh)', 4049: 'Bezug Low-Word (100 Wh)', 4050: 'Lieferung High-Word (100 Wh)',
  4051: 'Lieferung Low-Word (100 Wh)', 4061: 'Seriennummer High-Word', 4062: 'Seriennummer Low-Word',
  4063: 'Softwareversion × 100'
};

/** Wert, den der N43 für nicht definierte Größen sendet */
const LUMEL_UNDEFINED = 1e20;

/**
 * Lädt die ausgegebenen Lumel-Register von /api/lumel und stellt sie dar.
 * Läuft nur, wenn der Reiter Lumel sichtbar ist.
 * @returns {Promise<void>}
 */
async function loadLumelValues() {
  if (byId('t-lu').classList.contains('hide')) return;
  try {
    const lumel = await (await fetch('/api/lumel')).json();
    const sourceText = lumel.test ? '<span class="warn">Testmodus</span>'
      : lumel.silent ? '<span class="bad">keine aktuellen Daten – Simulation antwortet nicht</span>'
      : lumel.stale ? '<span class="warn">Daten veraltet</span>' : '<span class="ok">Daten aktuell</span>';
    byId('lumelInfo').innerHTML = (lumel.port
        ? `RTU${lumel.port} · Adresse ${lumel.address} · ${lumel.baud} ${escapeHtml(lumel.format)}`
          + (lumel.active ? '' : ' · <span class="warn">läuft nach Neustart</span>')
        : '<span class="warn">kein Port mit Rolle Lumel</span>')
      + ` · ${sourceText}` + (lumel.wordSwap ? ' · Wörter getauscht' : '');

    byId('lumelValues').innerHTML = lumel.values.map(([value, bitsHex], valueIndex) => {
      const [name, unit, decimals] = LUMEL_VALUE_NAMES[valueIndex] || ['nicht dokumentiert', '', 1];
      const valueText = value >= LUMEL_UNDEFINED * 0.99
        ? '<span class="mut" title="nicht definiert – der N43 sendet dann 1e20">n. def. (1e20)</span>'
        : formatNumber(value, decimals);
      return `<tr><td>${escapeHtml(name)}</td><td>${valueText}</td><td>${escapeHtml(unit)}</td>`
        + `<td>${7500 + valueIndex}</td><td>${7000 + 2 * valueIndex} / ${6000 + 2 * valueIndex}</td>`
        + `<td>${bitsHex.slice(0, 4)} ${bitsHex.slice(4)}</td></tr>`;
    }).join('');

    byId('lumelConfig').innerHTML = lumel.config.map(([registerAddress, value]) =>
      `<tr><td>${escapeHtml(LUMEL_CONFIG_NAMES[registerAddress] || '')}</td><td>${value}</td><td>${registerAddress}</td></tr>`
    ).join('');
  } catch (error) {}
}

// =====================================================================
// Status zyklisch abfragen und alle Statusanzeigen aktualisieren
// =====================================================================

/**
 * Erzeugt eine Zeile der Messwerttabelle mit den drei Phasenwerten und optional dem Gesamtwert.
 * @param {string} name  Bezeichnung der Messgröße
 * @param {number[]} phaseValues  Werte L1, L2, L3
 * @param {number|null} total  Gesamtwert oder null (leere Zelle)
 * @param {number} decimals  Nachkommastellen
 * @param {string} [unit]  Einheit für die Beschriftung
 * @returns {string} HTML der Tabellenzeile
 */
function renderMeterRow(name, phaseValues, total, decimals, unit) {
  return `<tr><td>${name}${unit ? ' [' + unit + ']' : ''}</td>`
    + phaseValues.map(value => `<td>${formatNumber(value, decimals)}</td>`).join('')
    + `<td>${total == null ? '' : formatNumber(total, decimals)}</td></tr>`;
}

/**
 * Erzeugt die Statuskarte eines RTU-Ports für den Reiter Übersicht
 * (Zähler, Fehler, Timing, letzte Anfrage/Antwort).
 * @param {Object} port  Port-Status aus /api/status
 * @param {number} index  Port-Index
 * @returns {string} HTML der Karte
 */
function renderPortStatus(port, index, allPorts) {
  const isTcp = port.role == 'goodwe-tcp';
  const portCount = allPorts.length;
  let details;
  if (port.role == 'off') {
    details = '<span>Status</span><span>deaktiviert</span>';
  } else {
    details =
      `<span>${port.role == 'lumel' ? 'Empfangen / Beantwortet' : 'Anfragen / Antworten'}</span><span>${port.requests} / ${port.responses}</span>`
      + `<span>Timeouts</span><span>${port.timeouts}</span>`
      + `<span>Exceptions</span><span>${port.exceptions}</span>`
      // Zeichen- und Zeitfehler gibt es nur auf der seriellen Leitung
      + (isTcp ? '' : `<span>CRC / Zeichen / t1,5-Fehler</span><span>${port.crc} / ${port.charErr} / ${port.gapErr}</span>`
                    + `<span>t1,5 / t3,5</span><span>${port.t15} / ${port.t35} µs</span>`)
      + `<span>Letzte Antwort</span><span>${port.lastOk < 0 ? 'nie' : formatAge(port.lastOk)}</span>`
      + (port.lastRequest ? `<span>Letzte Anfrage</span><span>${escapeHtml(port.lastRequest)}</span>` : '')
      + `<span>Letzter Fehler</span><span class="${port.lastError ? 'bad' : ''}">${escapeHtml(port.lastError || '–')}</span>`;
  }
  const title = isTcp ? 'Modbus TCP' : `RTU${index + 1} ${renderIdentifyButton(index)}`;
  const width = portCount == 3 ? 'span-4' : 'span-6';
  return `<div class="card ${width}">
    <div class="card-head"><h2>${title}</h2><span class="card-meta">${isTcp ? 'GoodWe · ' + escapeHtml(port.target) : ROLE_LABELS[port.role]}</span></div>
    <div class="kv">${details}</div>
  </div>`;
}

/** GPIO-Hinweise je Zielchip: [Pins, Text, CSS-Klasse] */
const GPIO_HINTS = {
  'ESP32-C6': [
    ['GPIO 12 / 13', 'USB D−/D+ (Konsole) – nicht wählbar'],
    ['GPIO 4, 5, 8, 9, 15', 'Strapping-Pins: nutzbar, aber beim Booten darf der Transceiver den Pegel nicht ziehen', 'warn'],
    ['GPIO 8', 'RGB-LED auf dem DevKitC-1'],
    ['GPIO 9', 'BOOT-Taster (5 s beim Start halten = Werksreset)'],
    ['GPIO 16 / 17', 'UART0 / USB-Seriell-Brücke auf dem DevKitC-1'],
    ['Empfehlung', 'RTU1 = 16 / 17 / 2, RTU2 = 18 / 19 / 20 oder 20 / 21 / 22']
  ],
  'ESP32-C3': [
    ['GPIO 18 / 19', 'USB D−/D+ (Konsole) – nicht wählbar'],
    ['GPIO 11–17', 'SPI-Flash – nicht wählbar'],
    ['GPIO 2, 8, 9', 'Strapping-Pins: nutzbar, aber beim Booten darf der Transceiver den Pegel nicht ziehen', 'warn'],
    ['GPIO 8', 'RGB-LED auf dem DevKitM-1'],
    ['GPIO 9', 'BOOT-Taster (5 s beim Start halten = Werksreset)'],
    ['GPIO 20 / 21', 'UART0 / USB-Seriell-Brücke auf manchen Boards'],
    ['Empfehlung', 'RTU1 = RX 21 / TX 20, RTU2 = RX 10 / TX 3, DE = Auto (Module mit automatischer Richtungsumschaltung)']
  ]
};
const GPIO_HINT_COMMON = [['DE/RE', '„Auto“ für RS485-Module mit automatischer Richtungsumschaltung']];

/**
 * Zeigt die GPIO-Hinweise für den Chip, auf dem die Firmware läuft (einmalig nach dem ersten Status).
 * @param {string} chipName  z. B. 'ESP32-C6'
 */
function renderGpioHints(chipName) {
  if (!chipName || byId('gpioHints').dataset.chip == chipName) return;
  byId('gpioHints').dataset.chip = chipName;
  byId('gpioHintTitle').textContent = 'Hinweise zu den GPIOs des ' + chipName;
  byId('gpioHints').innerHTML = [...(GPIO_HINTS[chipName] || []), ...GPIO_HINT_COMMON]
    .map(([pins, text, cssClass]) => `<tr><td>${pins}</td><td class="${cssClass || ''}">${escapeHtml(text)}</td></tr>`)
    .join('');
}

/**
 * Füllt die Statusleiste der Kopfzeile: Modbus-, MQTT- und VPN-Verbindung, Heap und Uhrzeit.
 * @param {Object} status  Antwort von /api/status
 */
function renderStatusBadges(status) {
  const clock = status.time;
  byId('hdrClock').innerHTML = clock.valid
    ? escapeHtml(clock.local.replace(/ [A-Z]+$/, ''))  // Zeitzonenkürzel weglassen
    : '<span class="warn">Uhrzeit nicht gestellt</span>';

  // Modbus: Verbindung zum GoodWe – aktuell, wenn die letzte Antwort höchstens 3 Intervalle zurückliegt
  const goodwePort = status.ports.find(port => port.role == 'goodwe' || port.role == 'goodwe-tcp');
  const modbusName = !goodwePort ? 'Modbus' : goodwePort.role == 'goodwe-tcp' ? 'Modbus TCP'
    : 'Modbus RTU' + (status.ports.indexOf(goodwePort) + 1);
  const maxAnswerAgeSec = Math.max(5, 3 * (config ? config.gwPollMs : 1000) / 1000);
  const modbusOk = goodwePort && goodwePort.lastOk >= 0 && goodwePort.lastOk <= maxAnswerAgeSec;
  byId('hdrModbus').innerHTML = !goodwePort ? `<span>${modbusName} aus</span>`
    : `<span class="${modbusOk ? 'ok' : 'bad'}">&#9679; ${modbusName}</span>`;
  byId('hdrModbus').title = goodwePort ? (goodwePort.lastError || 'keine Fehler') : 'kein GoodWe konfiguriert';

  const mqtt = status.mqtt;
  byId('hdrMqtt').innerHTML = !mqtt.enabled ? '<span>MQTT aus</span>'
    : `<span class="${mqtt.connected ? 'ok' : 'bad'}">&#9679; MQTT</span>`;

  // Heap: belegt und frei; unter 30 kB frei wird es für TLS (MQTT) und VPN eng
  const freeHeapKb = Math.round(status.heap / 1024);
  const totalHeapKb = Math.round((status.heapTotal || 0) / 1024);
  if (totalHeapKb) {
    byId('hdrHeapUsed').textContent = `Heap ${totalHeapKb - freeHeapKb} kB`;
    byId('hdrHeapUsed').title = `Belegter Heap von ${totalHeapKb} kB gesamt`;
  }
  byId('hdrHeap').innerHTML = `<span class="${freeHeapKb < 30 ? 'bad' : freeHeapKb < 50 ? 'warn' : ''}">Free Heap ${freeHeapKb} kB</span>`;
  if (status.heapMin) byId('hdrHeap').title = `Freier Heap, kleinster Wert seit dem Start: ${Math.round(status.heapMin / 1024)} kB`;

  const vpn = status.vpn;
  byId('hdrVpn').innerHTML = !vpn.enabled ? '<span>VPN aus</span>'
    : `<span class="${vpn.up ? 'ok' : 'warn'}">&#9679; VPN</span>`;
  byId('hdrVpn').title = vpn.enabled ? vpn.state : 'WireGuard deaktiviert';
}

/**
 * Fragt /api/status ab und aktualisiert alle Statusanzeigen: Kopfzeile, Übersicht,
 * Messwerte, Port-Statistik, WLAN, MQTT, OTA und System. Ist der ESP nicht
 * erreichbar, zeigt die Kopfzeile "Offline".
 * @returns {Promise<void>}
 */
async function pollStatus() {
  try {
    const status = await (await fetch('/api/status')).json();
    const meter = status.meter;
    const wifi = status.wifi;
    const goodwe = status.goodwe;

    // Kopfzeile: Zustand der Daten
    const dataCurrent = meter.test || meter.valid && meter.age < 10;
    byId('hdr').innerHTML = meter.test
      ? '<span class="warn">Testmodus</span>'
      : dataCurrent
        ? '<span class="ok">Daten aktuell</span>'
        : '<span class="bad">Keine GoodWe-Daten</span>';

    renderStatusBadges(status);
    renderGpioHints(status.chip);

    // Gerätename überall einsetzen, sobald er bekannt ist
    applyDeviceName(goodwe.model);

    // Übersicht: Kennzahl-Kacheln
    renderKpiTiles(status, dataCurrent);

    // Übersicht: Status
    renderKeyValueList('stat', {
      'WLAN': wifi.sta ? `${escapeHtml(wifi.ssid)} (${wifi.rssi} dBm)` : '<span class="bad">nicht verbunden</span>',
      'IP': wifi.ip || '–',
      ...(status.vpn.up ? { 'VPN-IP': escapeHtml(status.vpn.address.split('/')[0]) } : {}),
      'Access-Point': wifi.ap ? `${escapeHtml(wifi.apSsid)} – ${wifi.apIp}` : 'aus',
      'Datenalter': meter.age < 0 ? '–' : formatNumber(meter.age, 1) + ' s',
      'Laufzeit': formatAge(status.uptime),
      'Letzter Neustart': escapeHtml(status.diag.resetReason),
      'Letzter Absturz': status.diag.crashReport
        ? `<span class="bad">Task ${escapeHtml(status.diag.crashTask)}, PC ${escapeHtml(status.diag.crashPc)}</span>`
          + `<br><span class="hint">${escapeHtml(status.diag.crashBacktrace)}</span>`
        : 'keiner gespeichert'
    });

    // Übersicht: GoodWe
    renderKeyValueList('gw', {
      'Modell': escapeHtml(goodwe.model || '–'),
      'Seriennr.': escapeHtml(goodwe.serial || '–'),
      'Firmware': escapeHtml(goodwe.firmware || '–'),
      ...(goodwe.inverter ? {
        'PV gesamt': formatNumber(goodwe.pvTotal, 0) + ' W',
        'PV1…4': goodwe.pv.map(value => formatNumber(value, 0)).join(' / ') + ' W'
      } : { 'Wechselrichter': 'abgeschaltet' }),
      ...(goodwe.batt1 ? {
        'Batterie 1': `${formatNumber(goodwe.battV)} V · ${formatNumber(goodwe.battI)} A · ${formatNumber(goodwe.battP, 0)} W`
          + (goodwe.battSoc >= 0 ? ` · ${formatNumber(goodwe.battSoc, 0)} %` : '')
      } : {}),
      ...(goodwe.batt2 ? {
        'Batterie 2': `${formatNumber(goodwe.batt2V)} V · ${formatNumber(goodwe.batt2I)} A · ${formatNumber(goodwe.batt2P, 0)} W`
          + (goodwe.batt2Soc >= 0 ? ` · ${formatNumber(goodwe.batt2Soc, 0)} %` : '')
      } : {}),
      // tatsächlich gemessener Abstand der Abfragen; ist er größer als eingestellt, bremst die Verbindung
      'Intervall 1 (schnell)': goodwe.cycleMs
        ? `alle ${goodwe.cycleMs} ms` + (config ? ` <span class="hint">(eingestellt ${config.gwPollMs} ms)</span>` : '') : '–',
      'Intervall 2 (alle)': goodwe.slowCycleMs
        ? `alle ${goodwe.slowCycleMs} ms` + (config ? ` <span class="hint">(eingestellt ${config.gwSlowPollMs} ms)</span>` : '') : '–',
      ...(goodwe.inverter ? {
        'Temperatur': formatNumber(goodwe.temp) + ' °C',
        'Energie heute / gesamt': formatNumber(goodwe.eDay) + ' / ' + formatNumber(goodwe.eTotal) + ' kWh'
      } : {}),
      'Meter-Register': goodwe.meterRegs ? goodwe.meterRegs + ' (Komm.-Status ' + goodwe.meterComm + ')' : '–'
    });

    // Übersicht: Messwerttabelle
    byId('mt').innerHTML =
        renderMeterRow('Spannung', meter.u, null, 1, 'V')
      + renderMeterRow('Strom', meter.i, null, 2, 'A')
      + renderMeterRow('Wirkleistung', meter.p, meter.pTot, 0, 'W')
      + renderMeterRow('Blindleistung', meter.q, meter.qTot, 0, 'var')
      + renderMeterRow('Scheinleistung', meter.s, meter.sTot, 0, 'VA')
      + renderMeterRow('Leistungsfaktor', meter.pf, meter.pfTot, 3)
      + renderMeterRow('U L-L (12/23/31)', meter.uLL, null, 1, 'V')
      + `<tr><td>Frequenz [Hz]</td><td colspan="4">${formatNumber(meter.f, 2)}</td></tr>`
      + `<tr><td>Energie Bezug / Lieferung [kWh]</td><td colspan="4">${formatNumber(meter.eImp, 1)} / ${formatNumber(meter.eExp, 1)}</td></tr>`;

    // Übersicht: Port-Statistik
    byId('portstat').innerHTML = status.ports.map(renderPortStatus).join('');

    // WLAN
    renderKeyValueList('wstat', {
      'Status': wifi.sta ? '<span class="ok">verbunden</span>' : '<span class="bad">getrennt</span>',
      'SSID': escapeHtml(wifi.ssid || '–'),
      'IP': wifi.ip || '–',
      'Signal': wifi.sta ? wifi.rssi + ' dBm' : '–',
      'Sendeleistung': wifi.txPower ? 'max. ' + formatNumber(wifi.txPower, 2) + ' dBm' : '–',
      'Hostname': escapeHtml(wifi.hostname) + '.local',
      'MAC': wifi.mac
    });

    // MQTT
    const mqtt = status.mqtt;
    const ota = status.ota;
    const baseTopic = escapeHtml(mqtt.base);
    renderKeyValueList('mqstat', {
      'Verbindung': !mqtt.enabled
        ? 'deaktiviert'
        : mqtt.connected ? '<span class="ok">verbunden</span>' : '<span class="bad">getrennt</span>',
      'Broker': escapeHtml(mqtt.broker),
      'Node-ID': escapeHtml(mqtt.node),
      'Gesendet': mqtt.published + ' Nachrichten',
      'Verbindungen': mqtt.connects,
      'Letzter Fehler': escapeHtml(mqtt.lastError || '–')
    });
    renderKeyValueList('mqtopics', {
      'Status (LWT)': baseTopic + '/status',
      'Alle GoodWe-Werte (JSON)': baseTopic + '/goodwe/state',
      'Einzelwerte': baseTopic + '/goodwe/&lt;id&gt;',
      'Lumel-Ausgabewerte': baseTopic + '/meter/state',
      'Diagnose': baseTopic + '/bridge/state'
    });

    // Firmware-Update (OTA)
    renderKeyValueList('otastat', {
      'Version': escapeHtml(ota.version) + ' (' + escapeHtml(ota.build) + ')',
      'Partition': escapeHtml(ota.partition) + ' → n&auml;chstes Update in ' + escapeHtml(ota.nextPartition),
      'Status': ota.pending
        ? `<span class="warn">auf Bew&auml;hrung – Best&auml;tigung in ${ota.confirmIn} s</span>`
        : '<span class="ok">best&auml;tigt</span>',
      'Vorherige Version': escapeHtml(ota.previous || '–'),
      'Letztes Ergebnis': (ota.rolledBack
          ? '<span class="warn">Vom Bootloader verworfen: ' + escapeHtml(ota.invalidVersion) + '</span><br>'
          : '')
        + escapeHtml(ota.lastResult || '–')
    });
    byId('rbbtn').disabled = !ota.canRollback;

    // VPN
    const vpn = status.vpn;
    const vpnStateClass = vpn.up ? 'ok' : vpn.state == 'Fehler' ? 'bad' : vpn.enabled ? 'warn' : '';
    renderKeyValueList('vpnstat', {
      'Zustand': `<span class="${vpnStateClass}">${escapeHtml(vpn.state)}</span>`,
      'Verbunden seit': vpn.up ? formatAge(vpn.upSince) : '–',
      'Letzter Handshake': vpn.handshakeAgo >= 0 ? 'vor ' + formatAge(vpn.handshakeAgo) : '–',
      'Tunnel-Adresse': escapeHtml(vpn.address || '–'),
      'Gegenstelle': vpn.enabled ? escapeHtml(vpn.endpoint) : '–',
      'Netze': escapeHtml(vpn.allowedIps || '–'),
      'Uhrzeit gestellt': vpn.timeValid ? '<span class="ok">ja</span>' : '<span class="warn">nein</span>',
      'Letzter Fehler': `<span class="${vpn.lastError ? 'bad' : ''}">${escapeHtml(vpn.lastError || '–')}</span>`
    });

    // Modbus-TCP-Bridge
    const bridge = status.bridge;
    renderKeyValueList('bridgestat', {
      'Zustand': !bridge.enabled ? 'aus'
        : bridge.running ? `<span class="ok">aktiv auf Port ${bridge.port}</span>`
        : '<span class="bad">Port konnte nicht ge&ouml;ffnet werden</span>',
      'Adresse': bridge.enabled ? escapeHtml((status.wifi.ip || status.wifi.hostname) + ':' + bridge.port) : '–',
      'Verbundene Clients': bridge.clients.length ? bridge.clients.map(escapeHtml).join(', ') : '–',
      'Anfragen': bridge.requests,
      'Nicht weitergeleitet': `<span class="${bridge.errors ? 'warn' : ''}">${bridge.errors}</span>`,
      'Verbindungen seit Start': bridge.connections
    });

    // Uhrzeit
    const clock = status.time;
    renderKeyValueList('timestat', {
      'Ortszeit': clock.valid ? escapeHtml(clock.local) : '<span class="warn">noch nicht gestellt</span>',
      'Letzter Abgleich': clock.lastSyncAgo < 0 ? 'noch keiner' : 'vor ' + formatAge(clock.lastSyncAgo)
    });

    // System
    renderKeyValueList('sys', {
      'Chip': escapeHtml(status.chip || '–'),
      'Firmware': status.fw,
      'Freier Heap': Math.round(status.heap / 1024) + ' kB',
      'Laufzeit': formatAge(status.uptime)
    });
  } catch (error) {
    byId('hdr').innerHTML = '<span class="bad">Offline</span>';
  }
}


// =====================================================================
// Konsole: Live-Log über WebSocket, Befehle senden, Strg+A
// =====================================================================

/** Maximale Zeichenzahl in der Ansicht; ältere Ausgaben werden vorne abgeschnitten. */
const CONSOLE_MAX_CHARACTERS = 300000;

/** Offene WebSocket-Verbindung (oder null). */
let consoleSocket = null;
/** Timer für den automatischen Neuaufbau nach einem Verbindungsabbruch. */
let consoleReconnectTimer = null;
/** Bisher eingegebene Befehle (neuester zuletzt) und Position beim Blättern. */
const commandHistory = [];
let commandHistoryIndex = 0;
/** Rest einer unvollständigen Zeile aus dem letzten Datenpaket. */
let pendingPartialLine = '';

/**
 * Ordnet einer Logzeile eine Farbklasse zu (Fehler, Warnung, eigener Befehl, Modbus-Frame).
 * @param {string} line  eine Zeile ohne Zeilenumbruch
 * @returns {string} CSS-Klasse oder ''
 */
function consoleLineClass(line) {
  if (line.startsWith('E (')) return 'log-error';
  if (line.startsWith('W (')) return 'log-warn';
  if (line.startsWith('bridge> ')) return 'log-command';
  if (/^I \([^)]*\) (RTU\d|TCP):/.test(line)) return 'log-modbus';
  return '';
}

/**
 * Hängt empfangenen Text an die Konsolenansicht an. Vollständige Zeilen werden farbig
 * markiert; eine angefangene Zeile wartet auf das nächste Datenpaket.
 * @param {string} text  neuer Text vom Gerät
 */
function appendConsoleText(text) {
  const output = byId('consoleOutput');
  const lines = (pendingPartialLine + text).split('\n');
  pendingPartialLine = lines.pop();
  const fragment = document.createDocumentFragment();
  for (const line of lines) {
    const lineElement = document.createElement('span');
    const cssClass = consoleLineClass(line);
    if (cssClass) lineElement.className = cssClass;
    lineElement.textContent = line + '\n';
    fragment.appendChild(lineElement);
  }
  output.appendChild(fragment);
  // Speicher im Browser begrenzen: älteste Zeilen entfernen
  while (output.textContent.length > CONSOLE_MAX_CHARACTERS && output.firstChild) {
    output.removeChild(output.firstChild);
  }
  if (byId('consoleAutoscroll').checked) output.scrollTop = output.scrollHeight;
}

/**
 * Zeigt den Verbindungszustand der Konsole in der Kopfzeile des Reiters an.
 * @param {string} text  Zustandstext
 * @param {string} cssClass  'ok', 'bad' oder ''
 */
function setConsoleState(text, cssClass) {
  byId('consoleState').innerHTML = `<span class="${cssClass}">${text}</span>`;
}

/**
 * Baut die WebSocket-Verbindung zur Konsole auf. Bei einem Abbruch wird nach 3 s
 * automatisch neu verbunden (z. B. nach einem Neustart des Geräts).
 */
function connectConsole() {
  if (consoleSocket) return;
  clearTimeout(consoleReconnectTimer);
  setConsoleState('verbinde …', '');
  const socket = new WebSocket(`ws://${location.host}/ws/console`);
  consoleSocket = socket;
  socket.onopen = () => {
    setConsoleState('verbunden', 'ok');
    byId('consoleOutput').textContent = '';  // das Gerät schickt den gesamten Verlauf neu
    pendingPartialLine = '';
  };
  socket.onmessage = event => appendConsoleText(event.data);
  socket.onclose = () => {
    consoleSocket = null;
    setConsoleState('getrennt', 'bad');
    consoleReconnectTimer = setTimeout(connectConsole, 3000);
  };
  socket.onerror = () => socket.close();
}

/**
 * Sendet die eingegebene Befehlszeile an das Gerät (Formular-Submit) und merkt sie im Verlauf.
 * @param {Event} event  Submit-Ereignis des Formulars
 */
function sendConsoleCommand(event) {
  event.preventDefault();
  const input = byId('consoleCommand');
  const commandLine = input.value.trim();
  if (!commandLine) return;
  if (!consoleSocket || consoleSocket.readyState !== WebSocket.OPEN) {
    showToast('Konsole nicht verbunden');
    return;
  }
  consoleSocket.send(commandLine);
  if (commandHistory[commandHistory.length - 1] !== commandLine) commandHistory.push(commandLine);
  commandHistoryIndex = commandHistory.length;
  input.value = '';
  byId('consoleAutoscroll').checked = true;
}

/**
 * Schaltet die Protokollierung des Modbus-Verkehrs am Gerät ein oder aus ("trace on/off").
 * @param {boolean} enabled  true = jeden Modbus-Frame anzeigen
 */
function setModbusTrace(enabled) {
  if (consoleSocket && consoleSocket.readyState === WebSocket.OPEN) {
    consoleSocket.send(enabled ? 'trace on' : 'trace off');
  } else {
    showToast('Konsole nicht verbunden');
    byId('consoleTrace').checked = !enabled;
  }
}

/** Leert nur die Ansicht im Browser; der Puffer im Gerät bleibt erhalten. */
function clearConsoleView() {
  byId('consoleOutput').textContent = '';
  pendingPartialLine = '';
}

/** Speichert den aktuellen Konsoleninhalt als Textdatei. */
function downloadConsoleLog() {
  const text = byId('consoleOutput').textContent + pendingPartialLine;
  const link = document.createElement('a');
  link.href = URL.createObjectURL(new Blob([text], { type: 'text/plain;charset=utf-8' }));
  const timestamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');
  link.download = `modbus-bridge-log-${timestamp}.txt`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 1000);
}

/** Speichert die WireGuard-Einstellungen (Neustart folgt automatisch). */
function saveVpn() {
  saveConfig({
    wgEnabled: byId('wgEnabled').checked,
    wgAddress: byId('wgAddress').value.trim(),
    wgPrivateKey: byId('wgPrivateKey').value.trim(),
    wgPeerPublicKey: byId('wgPeerPublicKey').value.trim(),
    wgPresharedKey: byId('wgPresharedKey').value.trim(),
    wgPresharedKeyClear: !byId('wgPresharedKey').value.trim(),
    wgEndpoint: byId('wgEndpoint').value.trim(),
    wgPort: +byId('wgPort').value,
    wgAllowedIps: byId('wgAllowedIps').value.trim(),
    wgKeepalive: +byId('wgKeepalive').value
  });
}

/** Speichert NTP-Server und Zeitzone (Neustart folgt automatisch). */
function saveTime() {
  saveConfig({
    ntpServer1: byId('ntpServer1').value.trim(),
    ntpServer2: byId('ntpServer2').value.trim(),
    timeZone: byId('timeZone').value.trim()
  });
}

/**
 * Liest eine WireGuard-Konfiguration im wg-quick-Format und füllt die Formularfelder.
 * Mehrere Adressen/Netze werden übernommen, IPv6-Einträge ausgelassen (der ESP nutzt nur IPv4).
 * @param {string} text  Inhalt der .conf-Datei
 * @returns {number} Anzahl übernommener Felder
 */
function parseWireguardConfig(text) {
  const values = {};
  let section = '';
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.replace(/#.*/, '').trim();
    const sectionMatch = line.match(/^\[(\w+)\]$/);
    if (sectionMatch) { section = sectionMatch[1].toLowerCase(); continue; }
    const keyValue = line.match(/^(\w+)\s*=\s*(.+)$/);
    if (keyValue) values[section + '.' + keyValue[1].toLowerCase()] = keyValue[2].trim();
  }
  const ipv4Only = list => (list || '').split(',').map(entry => entry.trim()).filter(entry => entry && !entry.includes(':')).join(', ');
  const fieldValues = {
    wgPrivateKey: values['interface.privatekey'],
    wgAddress: ipv4Only(values['interface.address']).split(',')[0],
    wgPeerPublicKey: values['peer.publickey'],
    wgPresharedKey: values['peer.presharedkey'],
    wgAllowedIps: ipv4Only(values['peer.allowedips']),
    wgKeepalive: values['peer.persistentkeepalive']
  };
  const endpoint = values['peer.endpoint'];
  if (endpoint) {
    const separator = endpoint.lastIndexOf(':');
    fieldValues.wgEndpoint = separator > 0 ? endpoint.slice(0, separator) : endpoint;
    if (separator > 0) fieldValues.wgPort = endpoint.slice(separator + 1);
  }
  let applied = 0;
  for (const [fieldId, value] of Object.entries(fieldValues)) {
    if (value) { byId(fieldId).value = value; applied++; }
  }
  return applied;
}

/** Übernimmt den eingefügten Konfigurationstext in die Felder. */
function applyWireguardImport() {
  const applied = parseWireguardConfig(byId('wgImport').value);
  showToast(applied ? `${applied} Felder übernommen – bitte prüfen und speichern` : 'Keine WireGuard-Konfiguration erkannt');
  if (applied) byId('wgEnabled').checked = true;
}

/**
 * Liest eine .conf-Datei ein und übernimmt sie direkt in die Felder.
 * @param {HTMLInputElement} fileInput  das Dateifeld
 */
function loadWireguardFile(fileInput) {
  const file = fileInput.files[0];
  if (!file) return;
  file.text().then(text => {
    byId('wgImport').value = text;
    applyWireguardImport();
  });
  fileInput.value = '';
}

/**
 * Zeigt die Felder für Modbus TCP nur, wenn diese Anbindung gewählt ist.
 */
function updateTransportVisibility() {
  const viaTcp = byId('gwTransport').value == 'tcp';
  byId('tcpbox').classList.toggle('hide', !viaTcp);
  // RS485-Ports: Rolle GoodWe sperren bzw. freigeben, solange die Anbindung TCP ist
  [0, 1].forEach(index => {
    const roleSelect = byId('role' + index);
    if (!roleSelect) return;
    const goodweOption = roleSelect.querySelector('option[value=goodwe]');
    goodweOption.disabled = viaTcp;
    goodweOption.textContent = ROLE_LABELS.goodwe + (viaTcp ? ' – GoodWe läuft über Modbus TCP' : '');
    if (viaTcp && roleSelect.value == 'goodwe') {
      roleSelect.value = 'off';
      updateAddressLabel(index);
    }
  });
}

/** Zuletzt ausgelesener Gerätename (z. B. "GW25K-ET"), leer solange unbekannt. */
let deviceModel = '';

/**
 * Anzeigename des Wechselrichters: "GoodWe GW25K-ET", solange unbekannt nur "GoodWe".
 * @param {string} model  vom Gerät gelesener Modellname
 * @returns {string} Anzeigename
 */
function deviceDisplayName(model) {
  return model ? 'GoodWe ' + model : 'GoodWe';
}

/**
 * Setzt den ausgelesenen Gerätenamen überall dort ein, wo er angezeigt wird (Elemente mit Klasse
 * "device-name") sowie in der Rollen-Auswahl der RS485-Ports. Nur bei Änderung, damit offene
 * Auswahllisten nicht flackern.
 * @param {string} model  vom Gerät gelesener Modellname
 */
function applyDeviceName(model) {
  if (!model || model == deviceModel) return;
  deviceModel = model;
  const displayName = deviceDisplayName(model);
  document.querySelectorAll('.device-name').forEach(element => element.textContent = displayName);
  ROLE_LABELS.goodwe = displayName + ' auslesen (Master)';
  document.querySelectorAll('select[id^=role] option[value=goodwe]').forEach(option => {
    option.textContent = ROLE_LABELS.goodwe + (option.disabled ? ' – GoodWe läuft über Modbus TCP' : '');
  });
  document.title = 'Modbus-Bridge · ' + displayName;
}

/**
 * Füllt die vier Kennzahl-Kacheln der Übersicht (Netz, PV, Batterie, Verbindung).
 * @param {Object} status  Antwort von /api/status
 * @param {boolean} dataCurrent  true, wenn aktuelle GoodWe-Daten (oder Testmodus) vorliegen
 */
function renderKpiTiles(status, dataCurrent) {
  const meter = status.meter;
  const goodwe = status.goodwe;
  const watts = value => `${formatNumber(value, 0)}<small>W</small>`;

  // Netzleistung: die Bedeutung des Vorzeichens hängt von der Einstellung "Vorzeichen umkehren" ab
  const positiveMeansImport = !config || config.gwInvertSign;
  if (meter.valid || meter.test) {
    const total = Number(meter.pTot);
    const direction = total == 0 ? 'ausgeglichen'
      : (total > 0) == positiveMeansImport ? 'Bezug aus dem Netz' : 'Einspeisung ins Netz';
    byId('kpiGrid').innerHTML = watts(total);
    byId('kpiGridSub').textContent = direction;
  } else {
    byId('kpiGrid').textContent = '–';
    byId('kpiGridSub').textContent = 'keine Daten';
  }

  if (goodwe.valid) {
    byId('kpiPv').innerHTML = goodwe.inverter ? watts(goodwe.pvTotal) : '–';
    byId('kpiPvSub').textContent = goodwe.inverter ? `heute ${formatNumber(goodwe.eDay)} kWh` : 'Wechselrichter abgeschaltet';
    // Batterie: bei zwei Batterien Summe der Leistung und Ladezustand beider Batterien
    const totalBatteryPower = (goodwe.batt1 ? Number(goodwe.battP) : 0) + (goodwe.batt2 ? Number(goodwe.batt2P) : 0);
    byId('kpiBattery').innerHTML = watts(totalBatteryPower);
    // GoodWe: positive Batterieleistung = Entladen, negative = Laden
    const batteryState = byId('kpiBatteryState');
    const BATTERY_IDLE_WATT = 20;
    batteryState.classList.remove('hide');
    batteryState.innerHTML = totalBatteryPower < -BATTERY_IDLE_WATT ? '<span class="ok">&#9650; l&auml;dt</span>'
      : totalBatteryPower > BATTERY_IDLE_WATT ? '<span class="warn">&#9660; entl&auml;dt</span>'
      : '<span>Ruhe</span>';
    const socTexts = [];
    if (goodwe.batt1 && (goodwe.battSoc > 0 || !goodwe.batt2)) socTexts.push(goodwe.batt2 ? `B1 ${formatNumber(goodwe.battSoc, 0)} %` : `${formatNumber(goodwe.battSoc, 0)} %`);
    if (goodwe.batt2 && goodwe.batt2Soc >= 0) socTexts.push(`B2 ${formatNumber(goodwe.batt2Soc, 0)} %`);
    byId('kpiBatterySub').textContent = socTexts.length ? 'Ladezustand ' + socTexts.join(' · ')
      : goodwe.batt1 ? `${formatNumber(goodwe.battV)} V` : 'keine Batterie aktiv';
  } else {
    for (const id of ['kpiPv', 'kpiBattery']) byId(id).textContent = '–';
    byId('kpiBatteryState').classList.add('hide');
    byId('kpiPvSub').textContent = byId('kpiBatterySub').textContent = 'keine Daten';
  }

  const goodwePort = status.ports.find(port => port.role == 'goodwe' || port.role == 'goodwe-tcp');
  byId('kpiLink').innerHTML = meter.test ? '<span class="warn">Testmodus</span>'
    : dataCurrent ? '<span class="ok">Online</span>' : '<span class="bad">Keine Daten</span>';
  byId('kpiLinkSub').textContent = (goodwePort ? `${goodwePort.responses} / ${goodwePort.requests} Antworten` : 'kein GoodWe-Port')
    + ` · MQTT ${status.mqtt.connected ? 'verbunden' : status.mqtt.enabled ? 'getrennt' : 'aus'}`;
}

/**
 * Zeigt neben der eigenen Dateiauswahl den Namen der gewählten Datei an.
 * @param {HTMLInputElement} fileInput  das (versteckte) Dateifeld
 * @param {string} nameElementId  ID des Elements für den Dateinamen
 */
function showChosenFileName(fileInput, nameElementId) {
  const file = fileInput.files[0];
  byId(nameElementId).textContent = file ? `${file.name} (${Math.round(file.size / 1024)} kB)` : 'Keine Datei gewählt';
}

/**
 * Passt die Höhe der Konsolenausgabe so an, dass Ausgabe und Eingabezeile genau das
 * sichtbare Browserfenster ausfüllen (ohne dass die Seite selbst scrollt).
 * Wird beim Öffnen des Reiters, bei Größenänderung und beim Drehen des Geräts aufgerufen.
 */
function fitConsoleHeight() {
  const section = byId('t-co');
  if (section.classList.contains('hide')) return;
  const output = byId('consoleOutput');
  const card = output.closest('.card');
  const visibleHeight = window.visualViewport ? window.visualViewport.height : window.innerHeight;
  // Platz unter der Ausgabe: Eingabezeile + Innenabstand der Karte + Seitenrand unten
  const spaceBelowOutput = card.getBoundingClientRect().bottom - output.getBoundingClientRect().bottom
                         + parseFloat(getComputedStyle(document.querySelector('main')).paddingBottom);
  const outputTop = output.getBoundingClientRect().top;  // Abstand zum oberen Fensterrand
  const newHeight = Math.max(160, Math.floor(visibleHeight - outputTop - spaceBelowOutput));
  output.style.height = newHeight + 'px';
}
window.addEventListener('resize', fitConsoleHeight);
window.addEventListener('orientationchange', () => setTimeout(fitConsoleHeight, 300));
if (window.visualViewport) window.visualViewport.addEventListener('resize', fitConsoleHeight);

/**
 * Markiert den gesamten Konsoleninhalt (statt der ganzen Seite).
 */
function selectAllConsoleText() {
  const output = byId('consoleOutput');
  const range = document.createRange();
  range.selectNodeContents(output);
  const selection = window.getSelection();
  selection.removeAllRanges();
  selection.addRange(range);
}

// Strg+A / Cmd+A: im Konsolen-Reiter nur die Ausgabe markieren. In Eingabefeldern bleibt das
// normale Verhalten (Feldinhalt markieren) erhalten.
document.addEventListener('keydown', event => {
  const isSelectAll = (event.ctrlKey || event.metaKey) && !event.altKey && event.key.toLowerCase() === 'a';
  if (!isSelectAll || byId('t-co').classList.contains('hide')) return;
  const target = event.target;
  const isTextField = target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable;
  if (isTextField) return;
  event.preventDefault();
  selectAllConsoleText();
});

// Pfeil hoch/runter im Befehlsfeld: durch die letzten Befehle blättern
byId('consoleCommand').addEventListener('keydown', event => {
  if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
  if (!commandHistory.length) return;
  event.preventDefault();
  commandHistoryIndex += event.key === 'ArrowUp' ? -1 : 1;
  commandHistoryIndex = Math.max(0, Math.min(commandHistory.length, commandHistoryIndex));
  byId('consoleCommand').value = commandHistory[commandHistoryIndex] || '';
});

// Beim ersten Öffnen des Reiters verbinden; die Verbindung bleibt danach bestehen,
// damit beim Zurückwechseln nichts fehlt.
document.querySelector('[data-t=co]').addEventListener('click', () => {
  connectConsole();
  window.scrollTo(0, 0);
  fitConsoleHeight();
  setTimeout(() => {
    const output = byId('consoleOutput');
    if (byId('consoleAutoscroll').checked) output.scrollTop = output.scrollHeight;
  }, 0);
});

// Wer selbst nach oben scrollt, will lesen: Mitlaufen automatisch aus- bzw. am Ende wieder einschalten
byId('consoleOutput').addEventListener('scroll', () => {
  const output = byId('consoleOutput');
  const atBottom = output.scrollHeight - output.scrollTop - output.clientHeight < 30;
  byId('consoleAutoscroll').checked = atBottom;
});

// =====================================================================
// Start: Konfiguration laden, dann alle 2 s Status (und ggf. GoodWe-Werte) abfragen
// =====================================================================

loadConfig().then(pollStatus);
// Reiter aus der Adresszeile öffnen (z. B. http://modbus-bridge.local/#co für die Konsole)
const tabFromAddress = document.querySelector(`#nav button[data-t="${location.hash.slice(1)}"], #subnav button[data-t="${location.hash.slice(1)}"]`);
if (tabFromAddress) tabFromAddress.click();
setInterval(() => {
  pollStatus();
  loadGoodweValues();
  loadLumelValues();
}, 1000);
document.querySelector('[data-t=gw]').addEventListener('click', loadGoodweValues);
document.querySelector('[data-t=lu]').addEventListener('click', loadLumelValues);
</script>
</body>
</html>)HTML";
