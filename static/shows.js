"use strict";

// Step choices besides GIFs. Values are "effect:<name>" and "text:<style>".
const EFFECTS = [
  ["rain", "Digital rain", "#2f9e5b"],
  ["static", "Static", "#8a8f9c"],
  ["equalizer", "Equalizer", "#3aa6a0"],
  ["scanner", "Scanner", "#c0453d"],
  ["radar", "Radar", "#3f8f3f"],
  ["testpattern", "Test pattern", "#a557b5"],
  ["snow", "Snowfall", "#9db8d9"],
  ["confetti", "Confetti", "#d99a2b"],
  ["bounce", "Bouncing logo", "#5a73d6"],
];
const TEXT_STYLES = [
  ["marquee", "Marquee (scrolling)"],
  ["flash", "Flash words"],
  ["wave", "Wavy text"],
];
const TEXT_COLOR = "#c58b2f";

const state = {
  gifs: [],
  devices: [],
  shows: [],
  show: null, // {id|null, name, lanes: [{device, steps: [...]}]}
};

const $ = (selector) => document.querySelector(selector);
const statusEl = $("#status");

function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (key === "class") el.className = value;
    else if (key.startsWith("on")) el.addEventListener(key.slice(2), value);
    else if (value === true) el.setAttribute(key, "");
    else if (value !== false && value != null) el.setAttribute(key, value);
  }
  for (const child of children.flat()) {
    if (child == null || child === false) continue;
    el.append(child instanceof Node ? child : document.createTextNode(child));
  }
  return el;
}

function setStatus(text, isError = false) {
  statusEl.textContent = text;
  statusEl.classList.toggle("error", isError);
}

async function api(path, options = {}) {
  const response = await fetch(path, options);
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
  return data;
}

const jsonRequest = (method, body) => ({
  method,
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify(body),
});

const gifById = (id) => state.gifs.find((gif) => gif.id === id);

function gifHue(id) {
  let hash = 0;
  for (const ch of id) hash = (hash * 31 + ch.charCodeAt(0)) % 360;
  return hash;
}

const fmtSeconds = (s) => `${Math.round(s * 10) / 10}s`;

// ---- Library ----------------------------------------------------------

function renderLibrary() {
  const library = $("#library");
  library.replaceChildren();

  if (!state.gifs.length) {
    library.append(h("p", { class: "muted" }, "No GIFs yet. Add some above."));
    return;
  }

  for (const gif of state.gifs) {
    library.append(h("div", { class: "gif-card" },
      h("img", { src: `/api/gifs/${gif.id}/file`, alt: gif.name, loading: "lazy" }),
      h("div", { class: "name", title: gif.name }, gif.name),
      h("div", { class: "meta" }, `${gif.frame_count} frames · ${fmtSeconds(gif.duration_ms / 1000)}`),
      h("button", { type: "button", class: "danger", onclick: () => deleteGif(gif) }, "Delete"),
    ));
  }
}

async function deleteGif(gif) {
  if (!confirm(`Delete "${gif.name}" from the library?`)) return;
  try {
    await api(`/api/gifs/${gif.id}`, { method: "DELETE" });
    await loadGifs();
    renderEditor();
  } catch (error) {
    setStatus(error.message, true);
  }
}

$("#upload-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const files = [...$("#upload-file").files];
  const result = $("#upload-result");
  if (!files.length) {
    result.textContent = "Choose a GIF first.";
    return;
  }

  const failures = [];
  for (const [i, file] of files.entries()) {
    result.textContent = `Uploading ${i + 1} of ${files.length}…`;
    const form = new FormData();
    form.append("gif", file);
    try {
      await api("/api/gifs", { method: "POST", body: form });
    } catch (error) {
      failures.push(`${file.name}: ${error.message}`);
    }
  }

  result.textContent = failures.length ? failures.join("; ") : "Added.";
  $("#upload-file").value = "";
  await loadGifs();
  renderEditor();
});

// ---- Show editor ------------------------------------------------------

function laneSeconds(lane) {
  return lane.steps.reduce((sum, step) => sum + (Number(step.seconds) || 0), 0);
}

const cycleSeconds = () => Math.max(0, ...state.show.lanes.map(laneSeconds));

function defaultStep() {
  const gif = state.gifs[0];
  if (!gif) return { type: "wait", seconds: 5 };
  return { type: "gif", gif: gif.id, seconds: Math.max(1, Math.round(gif.duration_ms / 100) / 10) };
}

function newShow(device) {
  const used = device || state.devices[0]?.name || "";
  return { id: null, name: "", lanes: [{ device: used, steps: [defaultStep()] }] };
}

function stepValue(step) {
  if (step.type === "gif") return `gif:${step.gif}`;
  if (step.type === "effect") return `effect:${step.effect}`;
  if (step.type === "text") return `text:${step.style}`;
  return "wait";
}

function applyStepValue(step, value) {
  delete step.gif;
  delete step.effect;
  delete step.style;
  if (!value.startsWith("text:")) delete step.text;

  if (value.startsWith("text:")) {
    step.type = "text";
    step.style = value.slice(5);
    step.text = step.text || "";
  } else if (value.startsWith("gif:")) {
    step.type = "gif";
    step.gif = value.slice(4);
  } else if (value.startsWith("effect:")) {
    step.type = "effect";
    step.effect = value.slice(7);
  } else {
    step.type = "wait";
  }
}

function segmentStyle(step, total) {
  const width = total > 0 ? ((Number(step.seconds) || 0) / total) * 100 : 0;
  let background = "";
  if (step.type === "gif") background = `hsl(${gifHue(step.gif)} 55% 45%)`;
  if (step.type === "effect") background = (EFFECTS.find((e) => e[0] === step.effect) || [])[2] || "#8a8f9c";
  if (step.type === "text") background = TEXT_COLOR;
  return `width:${width}%;${background ? `background:${background}` : ""}`;
}

// Refresh the timelines and totals without rebuilding inputs (keeps focus).
function updateTotals() {
  const total = cycleSeconds();

  document.querySelectorAll(".lane").forEach((laneEl, laneIndex) => {
    const lane = state.show.lanes[laneIndex];
    laneEl.querySelector(".lane-total").textContent = `${fmtSeconds(laneSeconds(lane))} long`;

    laneEl.querySelector(".timeline").replaceChildren(
      ...lane.steps.map((step) => h("div", {
        class: `seg ${step.type === "wait" ? "wait" : ""}`,
        style: segmentStyle(step, total),
      })),
    );

    laneEl.querySelectorAll(".step").forEach((stepEl, stepIndex) => {
      const step = lane.steps[stepIndex];
      const gif = step.type === "gif" ? gifById(step.gif) : null;
      const loops = stepEl.querySelector(".loops");
      loops.textContent = gif && gif.duration_ms
        ? `≈ ${Math.round(((step.seconds * 1000) / gif.duration_ms) * 10) / 10} loops`
        : "";
    });
  });

  $("#cycle").textContent = total
    ? `The show loops every ${fmtSeconds(total)}; shorter lanes show their text message for the rest.`
    : "";
}

function deviceOptions(selected, taken) {
  const names = state.devices.map((d) => d.name);
  if (selected && !names.includes(selected)) names.push(selected);

  return names.map((name) => {
    const device = state.devices.find((d) => d.name === name);
    const label = device ? `${name} (${device.screen_type})` : `${name} (missing)`;
    return h("option", {
      value: name,
      selected: name === selected,
      disabled: taken.has(name) && name !== selected,
    }, label);
  });
}

function stepOptions(step) {
  const gifGroup = state.gifs.map((gif) =>
    h("option", { value: `gif:${gif.id}`, selected: stepValue(step) === `gif:${gif.id}` }, gif.name));
  const missing = step.type === "gif" && !gifById(step.gif)
    ? [h("option", { value: `gif:${step.gif}`, selected: true }, "(missing GIF)")]
    : [];

  const current = stepValue(step);
  return [
    h("option", { value: "wait", selected: current === "wait" }, "⏸ Wait (show the text message)"),
    h("optgroup", { label: "Effects" },
      ...EFFECTS.map(([id, label]) => h("option", { value: `effect:${id}`, selected: current === `effect:${id}` }, `✨ ${label}`))),
    h("optgroup", { label: "Text" },
      ...TEXT_STYLES.map(([id, label]) => h("option", { value: `text:${id}`, selected: current === `text:${id}` }, `💬 ${label}`))),
    h("optgroup", { label: "GIFs" }, ...gifGroup, ...missing),
  ];
}

function renderStep(lane, step, index) {
  const gif = step.type === "gif" ? gifById(step.gif) : null;
  const thumb = gif
    ? h("img", { class: "thumb", src: `/api/gifs/${gif.id}/file`, alt: "" })
    : h("span", { class: "thumb wait" }, { effect: "✨", text: "💬" }[step.type] || "⏸");

  const move = (delta) => {
    const target = index + delta;
    if (target < 0 || target >= lane.steps.length) return;
    [lane.steps[index], lane.steps[target]] = [lane.steps[target], lane.steps[index]];
    renderEditor();
  };

  return h("div", { class: "step" },
    thumb,
    h("select", {
      "aria-label": "Step",
      onchange: (event) => {
        applyStepValue(step, event.target.value);
        renderEditor();
      },
    }, stepOptions(step)),
    step.type === "text" ? h("input", {
      type: "text", class: "step-text", maxlength: "100", value: step.text || "",
      placeholder: "Text (blank = the display's message)", "aria-label": "Text",
      oninput: (event) => { step.text = event.target.value; },
    }) : null,
    h("input", {
      type: "number", min: "0.1", max: "3600", step: "0.1",
      value: step.seconds, "aria-label": "Seconds",
      oninput: (event) => {
        step.seconds = parseFloat(event.target.value);
        updateTotals();
      },
    }),
    h("span", { class: "muted" }, "sec"),
    h("span", { class: "loops" }),
    h("button", { type: "button", class: "secondary icon", title: "Move up", onclick: () => move(-1) }, "↑"),
    h("button", { type: "button", class: "secondary icon", title: "Move down", onclick: () => move(1) }, "↓"),
    h("button", {
      type: "button", class: "danger icon", title: "Remove step",
      onclick: () => {
        lane.steps.splice(index, 1);
        renderEditor();
      },
    }, "✕"),
  );
}

function renderLane(lane, laneIndex) {
  const taken = new Set(state.show.lanes.map((l) => l.device));

  return h("div", { class: "lane" },
    h("div", { class: "lane-header" },
      h("select", {
        "aria-label": "Display",
        onchange: (event) => { lane.device = event.target.value; renderEditor(); },
      }, deviceOptions(lane.device, taken)),
      h("span", { class: "muted lane-total" }),
      h("span", { class: "spacer" }),
      h("button", {
        type: "button", class: "danger icon", title: "Remove lane",
        onclick: () => { state.show.lanes.splice(laneIndex, 1); renderEditor(); },
      }, "Remove lane"),
    ),
    h("div", { class: "timeline" }),
    lane.steps.map((step, index) => renderStep(lane, step, index)),
    h("div", { class: "lane-footer" },
      h("button", {
        type: "button", class: "secondary",
        onclick: () => { lane.steps.push(defaultStep()); renderEditor(); },
      }, "+ Add step"),
    ),
  );
}

function renderToolbar() {
  const select = $("#show-select");
  select.replaceChildren(
    h("option", { value: "" }, "New show"),
    ...state.shows.map((show) => h("option", {
      value: show.id,
      selected: show.id === state.show.id,
    }, show.playing_on.length ? `▶ ${show.name}` : show.name)),
  );

  const saved = state.shows.find((show) => show.id === state.show.id);
  $("#show-name").value = state.show.name;
  $("#stop-btn").disabled = !(saved && saved.playing_on.length);
  $("#delete-btn").disabled = !saved;
  $("#add-lane-btn").disabled = state.show.lanes.length >= state.devices.length;
}

function renderEditor() {
  renderToolbar();
  $("#lanes").replaceChildren(...state.show.lanes.map(renderLane));
  renderLibrary();
  updateTotals();
}

$("#show-name").addEventListener("input", (event) => { state.show.name = event.target.value; });

$("#show-select").addEventListener("change", (event) => {
  const saved = state.shows.find((show) => show.id === event.target.value);
  state.show = saved ? structuredClone(saved) : newShow();
  setStatus(saved?.playing_on.length ? `Playing on ${saved.playing_on.join(", ")}.` : "");
  renderEditor();
});

$("#add-lane-btn").addEventListener("click", () => {
  const taken = new Set(state.show.lanes.map((lane) => lane.device));
  const free = state.devices.find((device) => !taken.has(device.name));
  if (!free) return;
  state.show.lanes.push({ device: free.name, steps: [defaultStep()] });
  renderEditor();
});

function payload() {
  return {
    name: state.show.name,
    lanes: state.show.lanes.map((lane) => ({
      device: lane.device,
      steps: lane.steps.map((step) => {
        const out = { type: step.type, seconds: step.seconds };
        if (step.type === "gif") out.gif = step.gif;
        if (step.type === "effect") out.effect = step.effect;
        if (step.type === "text") {
          out.style = step.style;
          out.text = step.text || "";
        }
        return out;
      }),
    })),
  };
}

async function save() {
  const saved = state.show.id
    ? await api(`/api/shows/${state.show.id}`, jsonRequest("PUT", payload()))
    : await api("/api/shows", jsonRequest("POST", payload()));
  await loadShows();
  state.show = structuredClone(state.shows.find((show) => show.id === saved.id));
  return saved;
}

async function run(action, message) {
  try {
    setStatus("Working…");
    const result = await action();
    renderEditor();
    setStatus(message ? message(result) : "Done.");
  } catch (error) {
    setStatus(error.message, true);
  }
}

$("#save-btn").addEventListener("click", () => run(save, () => "Saved."));

$("#play-btn").addEventListener("click", () => run(async () => {
  const saved = await save();
  const result = await api(`/api/shows/${saved.id}/play`, { method: "POST" });
  await loadShows();
  return result;
}, (result) => {
  const notes = result.lanes.flatMap((lane) => lane.notes.map((note) => `${lane.device}: ${note}`));
  return `Playing on ${result.lanes.map((lane) => lane.device).join(", ")}.` +
    (notes.length ? ` ${notes.join(". ")}.` : "");
}));

$("#stop-btn").addEventListener("click", () => run(async () => {
  const result = await api(`/api/shows/${state.show.id}/stop`, { method: "POST" });
  await loadShows();
  return result;
}, (result) => {
  if (!result.stopped.length) return "Nothing was playing.";
  const back = result.restored.length ? ` ${result.restored.join(", ")} went back to the GIF it was playing before.` : "";
  return `Stopped ${result.stopped.join(", ")}.${back}`;
}));

$("#delete-btn").addEventListener("click", async () => {
  if (!state.show.id || !confirm(`Delete show "${state.show.name}"?`)) return;
  await run(async () => {
    await api(`/api/shows/${state.show.id}`, { method: "DELETE" });
    await loadShows();
    state.show = newShow();
  }, () => "Deleted.");
});

// ---- Loading ----------------------------------------------------------

async function loadGifs() { state.gifs = (await api("/api/gifs")).gifs; }
async function loadDevices() { state.devices = (await api("/api/devices")).devices; }
async function loadShows() { state.shows = (await api("/api/shows")).shows; }

// Keep the ▶ markers and Stop button current without disturbing edits.
async function refreshPlayback() {
  try {
    await loadShows();
    renderToolbar();
  } catch { /* the server is briefly unreachable; try again next time */ }
}

async function init() {
  try {
    await Promise.all([loadGifs(), loadDevices(), loadShows()]);
  } catch (error) {
    setStatus(error.message, true);
    return;
  }

  const params = new URLSearchParams(location.search);
  const wanted = state.shows.find((show) => show.id === params.get("show"));
  state.show = wanted ? structuredClone(wanted) : newShow(params.get("device"));
  renderEditor();
  setInterval(refreshPlayback, 3000);
}

init();
