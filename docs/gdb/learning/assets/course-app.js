"use strict";

const app = {
  course: null,
  progress: null,
  selectedModule: "",
};

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

async function api(path, options = {}) {
  const headers = {...(options.headers || {})};
  if (options.body) headers["Content-Type"] = "application/json";
  const response = await fetch(path, {...options, headers});
  const payload = await response.json();
  if (!response.ok) throw new Error(payload.error || `请求失败：${response.status}`);
  return payload;
}

function stateLabel(state) {
  const labels = {
    Locked: "未解锁", NotStarted: "未开始", Learning: "学习中",
    Provisional: "暂时掌握", Mastered: "长期掌握", Planned: "待建设",
  };
  return labels[state] || state;
}

function moduleStatus(moduleId) {
  return app.progress.statuses[moduleId] || {state: "Planned", gate: {}};
}

function renderToday() {
  const panel = document.getElementById("today-panel");
  const now = new Date();
  const due = [];
  for (const module of app.course.modules) {
    const status = moduleStatus(module.id);
    for (const [day, value] of Object.entries(status.review_due || {})) {
      const completed = (status.reviews || {})[day]?.passed;
      if (!completed && new Date(value) <= now) due.push(`${module.id} · D+${day}`);
    }
  }
  const next = app.course.modules.find((module) => {
    const state = moduleStatus(module.id).state;
    return state === "NotStarted" || state === "Learning";
  });
  if (due.length) {
    panel.innerHTML = `<strong>今日复习：</strong>${due.map(escapeHtml).join("、")}`;
  } else if (next) {
    panel.innerHTML = `<strong>建议继续：</strong>${escapeHtml(next.id)} · ${escapeHtml(next.title)}`;
  } else {
    panel.textContent = "当前没有到期复习；18 个模块均已开放，请从尚未掌握的模块继续。";
  }
}

function renderModuleGrid() {
  const grid = document.getElementById("module-grid");
  grid.replaceChildren();
  for (const module of app.course.modules) {
    const status = moduleStatus(module.id);
    const card = document.createElement("article");
    card.className = `module-card state-${status.state.toLowerCase()}`;
    card.innerHTML = `
      <div class="module-meta"><span>${escapeHtml(module.id)}</span><span>${escapeHtml(stateLabel(status.state))}</span></div>
      <h3>${escapeHtml(module.title)}</h3>
      <p>${module.delivery_state === "current" ? "完整模块 · 掌握后推进" : "试学模块 · 掌握后推进"}</p>
      <button type="button" ${["Locked", "Planned"].includes(status.state) ? "disabled" : ""}>进入模块</button>`;
    card.querySelector("button").addEventListener("click", () => selectModule(module.id));
    grid.appendChild(card);
  }
}

function questionForm(module, kind, questions, title) {
  const blocks = questions.map((question, index) => `
    <fieldset>
      <legend>${index + 1}. ${escapeHtml(question.prompt)}</legend>
      <div class="answer-grid">${question.options.map((option) => `
        <label><input type="radio" name="${escapeHtml(question.id)}" value="${escapeHtml(option.id)}" required>
        <span>${escapeHtml(option.text)}</span></label>`).join("")}</div>
      ${questionAnswer(module, question)}
    </fieldset>`).join("");
  return `<form class="activity-form" data-action="answers" data-kind="${kind}">
    <h3>${escapeHtml(title)}</h3>${blocks}
    <button type="submit">提交${kind === "quiz" ? "验收测验" : "诊断"}</button>
    <output aria-live="polite"></output></form>`;
}

function questionAnswer(module, question) {
  const viewId = module.question_standard_views?.[question.id];
  const view = module.standard_views.find((item) => item.id === viewId);
  if (!view) return `<p class="error">本题缺少标准答案映射：${escapeHtml(question.id)}</p>`;
  const answer = question.answer_explanation || {};
  const correct = answer.correct_option && answer.correct_text
    ? `<p><strong>正确选项：${escapeHtml(answer.correct_option)} · ${escapeHtml(answer.correct_text)}</strong></p>`
    : "";
  const explanation = answer.explanation || view.standard_answer;
  return `<details class="question-answer" data-question-answer="${escapeHtml(question.id)}">
    <summary>查看本题对应标准答案：${escapeHtml(view.title)}</summary>
    ${correct}
    <p>${escapeHtml(explanation)}</p>
    <p class="small">完整图表、证据边界和来源见下方“标准答案与结构图”。</p>
  </details>`;
}

function graphSvg(diagram) {
  const nodes = Object.fromEntries(diagram.nodes.map((node) => [node.id, node]));
  const edges = diagram.edges.map((edge) => {
    const from = nodes[edge.from];
    const to = nodes[edge.to];
    const x1 = from.x + from.w / 2;
    const y1 = from.y + from.h / 2;
    const x2 = to.x + to.w / 2;
    const y2 = to.y + to.h / 2;
    return `<line class="answer-edge" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"></line>
      <text class="answer-edge-label" x="${(x1 + x2) / 2}" y="${(y1 + y2) / 2 - 5}">${escapeHtml(edge.label)}</text>`;
  }).join("");
  const renderedNodes = diagram.nodes.map((node) => {
    const lines = String(node.label).split("\n");
    const text = lines.map((line, index) => `<tspan x="${node.x + node.w / 2}" dy="${index ? 20 : 0}">${escapeHtml(line)}</tspan>`).join("");
    return `<g class="answer-node node-${escapeHtml(node.layer)}">
      <rect x="${node.x}" y="${node.y}" width="${node.w}" height="${node.h}" rx="10"></rect>
      <text x="${node.x + node.w / 2}" y="${node.y + node.h / 2 - (lines.length - 1) * 10}" text-anchor="middle">${text}</text>
    </g>`;
  }).join("");
  return `<svg class="standard-svg" viewBox="${escapeHtml(diagram.viewBox)}" role="img" aria-label="标准关系图"><defs><marker id="answer-arrow" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L0,6 L6,3 z" fill="#8da49d"></path></marker></defs>${edges}${renderedNodes}</svg>`;
}

function bytesTable(diagram) {
  const rows = diagram.rows.map((row) => `<tr>
    <th>${escapeHtml(row.label)}</th><td><code>${escapeHtml(row.bytes)}</code></td>
    <td>${escapeHtml(row.decode)}</td><td>${escapeHtml(row.conclusion)}</td></tr>`).join("");
  return `<div class="table-scroll"><table class="standard-table"><thead><tr><th>编码</th><th>输入</th><th>解码</th><th>结论</th></tr></thead><tbody>${rows}</tbody></table></div>`;
}

function fileFamilyTable(diagram) {
  const rows = diagram.items.map((item) => `<tr>
    <th><code>${escapeHtml(item.name)}</code></th><td>${escapeHtml(item.role)}</td>
    <td>${escapeHtml(item.query)}</td><td>${escapeHtml(item.required)}</td></tr>`).join("");
  return `<div class="table-scroll"><table class="standard-table"><thead><tr><th>文件</th><th>职责</th><th>查询/生命周期</th><th>定位</th></tr></thead><tbody>${rows}</tbody></table></div>`;
}

function genericTable(diagram) {
  const headers = diagram.columns.map((column) => `<th>${escapeHtml(column)}</th>`).join("");
  const rows = diagram.rows.map((row) => `<tr>${row.map((cell) =>
    `<td>${escapeHtml(cell)}</td>`).join("")}</tr>`).join("");
  return `<div class="table-scroll"><table class="standard-table"><thead><tr>${headers}</tr></thead><tbody>${rows}</tbody></table></div>`;
}

function standardDiagram(view) {
  if (view.kind === "graph") return graphSvg(view.diagram);
  if (view.kind === "bytes") return bytesTable(view.diagram);
  if (view.kind === "file-family") return fileFamilyTable(view.diagram);
  if (view.kind === "table") return genericTable(view.diagram);
  return `<p class="error">未知标准答案图类型：${escapeHtml(view.kind)}</p>`;
}

function evidenceList(items, className) {
  return `<ul class="${className}">${items.map((item) => {
    const text = typeof item === "string" ? item : `${item.level}：${item.claim}`;
    return `<li>${escapeHtml(text)}</li>`;
  }).join("")}</ul>`;
}

function standardAnswerPanel(module, status) {
  const viewed = status.standard_views || {};
  const views = module.standard_views.map((view) => `
    <details id="standard-${escapeHtml(view.id)}" class="standard-answer" data-standard-view="${escapeHtml(view.id)}">
      <summary>查看标准答案：${escapeHtml(view.title)}
        <span class="view-status">${viewed[view.id] ? "已查看" : "未查看"}</span></summary>
      <p class="small">${escapeHtml(view.summary)}</p>
      ${standardDiagram(view)}
      <h4>标准结论</h4><p class="standard-answer-text">${escapeHtml(view.standard_answer)}</p>
      <div class="answer-columns">
        <section><h4>证据依据</h4>${evidenceList(view.evidence, "evidence-list")}</section>
        <section><h4>不能证明</h4>${evidenceList(view.not_proven, "boundary-list")}</section>
        <section><h4>常见误解</h4>${evidenceList(view.common_mistakes, "mistake-list")}</section>
      </div>
      <p class="small">来源：${view.sources.map((source) => `<a href="${escapeHtml(source.href)}" target="_blank" rel="noopener">${escapeHtml(source.label)}</a>`).join("、")}</p>
    </details>`).join("");
  return `<section class="standard-answer-panel"><div class="section-heading"><h3>标准答案与结构图</h3><span class="small" data-standard-count>${Object.keys(viewed).length}/${module.standard_views.length} 已查看</span></div>
    <p class="small">答案默认收起。先阅读标准结论，再做测验；查看答案会记录学习轨迹，但不直接计入掌握门槛。</p>${views}</section>`;
}

function learningSteps(module, status) {
  const units = module.micro_units.map((unit) => `<li>
    <a href="${escapeHtml(module.lesson)}#${escapeHtml(unit.id)}" target="_blank" rel="noopener">${escapeHtml(unit.title)}</a>
    <span>${unit.minutes} 分钟</span></li>`).join("");
  const lab = module.labs[0];
  const gate = status.gate || {};
  const artifact = status.artifacts?.labs?.[lab.id];
  return `<section class="learning-steps"><h3>微型课程</h3><ol>${units}</ol></section>
    <section class="lab-panel"><h3>真实数据实验</h3><p>${escapeHtml(lab.title)}</p>
    <pre>python3 tools/learning/course.py lab ${lab.id}</pre>
    <p class="small">实验必须在仓库根目录运行。网页不执行 Shell；完成后点击“刷新进度”。</p>
    <button type="button" data-action="refresh">刷新进度</button>
    <span class="gate-result">${gate.labs_passed ? "实验产物已校验" : "尚未完成"}</span>
    ${artifact ? `<code>build/learning/artifacts/${escapeHtml(artifact)}</code>` : ""}</section>`;
}

function evidenceForm(module, status) {
  const artifact = status.artifacts?.evidence;
  return `<form class="activity-form" data-action="evidence"><h3>证据核查</h3>
    <p>${escapeHtml(module.evidence_task.prompt)}</p>
    <textarea name="response" rows="5" minlength="20" required placeholder="写明证据等级、结论，以及该证据不能证明什么。"></textarea>
    <button type="submit">保存证据说明</button><output aria-live="polite"></output>
    ${artifact ? `<code>build/learning/artifacts/${escapeHtml(artifact)}</code>` : ""}</form>`;
}

function oralPanel(module, status) {
  return `<section class="oral-panel"><h3>AI 口试</h3>
    <p>先生成口试包，复制到当前 Codex 对话。完成追问后，让 AI 按模板保存结果，再导入。</p>
    <pre>python3 tools/learning/course.py review-export ${module.id}
python3 tools/learning/course.py review-import ${module.id} build/learning/reviews/${module.id}-result.json</pre>
    <p class="gate-result">${status.gate.oral_review_passed ? "口试已通过" : "口试尚未通过"}</p></section>`;
}

function reviewForms(module, status) {
  if (status.state !== "Provisional" && status.state !== "Mastered") return "";
  const questionMap = Object.fromEntries(module.quiz_questions.map((question) => [question.id, question]));
  const now = new Date();
  return module.review_tasks.map((task) => {
    const completed = status.reviews?.[String(task.day)]?.passed;
    const due = new Date(status.review_due[String(task.day)]);
    if (completed) return `<div class="review-card"><strong>D+${task.day}</strong> 已通过</div>`;
    if (due > now) return `<div class="review-card"><strong>D+${task.day}</strong> ${due.toLocaleString("zh-CN")} 到期</div>`;
    const questions = task.question_ids.map((id) => questionMap[id]);
    return questionForm(module, `review-${task.day}`, questions, `D+${task.day} 闭卷复习`);
  }).join("");
}

function gateChecklist(status) {
  const gate = status.gate || {};
  const items = [
    ["quiz_passed", "测验与关键题"],
    ["labs_passed", "真实实验"], ["evidence_submitted", "证据作品"],
    ["oral_review_passed", "AI 口试"],
  ];
  return `<ul class="gate-list">${items.map(([key, label]) =>
    `<li>${gate[key] ? "✓" : "○"} ${label}</li>`).join("")}</ul>`;
}

function bindWorkspace(module) {
  const workspace = document.getElementById("module-workspace");
  workspace.querySelectorAll("form[data-action]").forEach((form) => {
    form.addEventListener("submit", (event) => submitActivity(event, module));
  });
  workspace.querySelectorAll("button[data-action='refresh']").forEach((button) => {
    button.addEventListener("click", refreshProgress);
  });
  bindStandardViews(module);
}

function selectModule(moduleId) {
  app.selectedModule = moduleId;
  const module = app.course.modules.find((item) => item.id === moduleId);
  const status = moduleStatus(moduleId);
  const workspace = document.getElementById("module-workspace");
  workspace.innerHTML = `<article class="module-workspace">
    <p class="kicker">${module.id} · ${escapeHtml(stateLabel(status.state))}</p><h2>${escapeHtml(module.title)}</h2>
    ${gateChecklist(status)}
    ${learningSteps(module, status)}
    ${questionForm(module, "diagnostic", module.diagnostic_questions, "课前诊断")}
    ${standardAnswerPanel(module, status)}
    ${questionForm(module, "quiz", module.quiz_questions, "模块验收测验")}
    ${evidenceForm(module, status)}${oralPanel(module, status)}
    <section><h3>间隔复习</h3>${reviewForms(module, status) || "达到暂时掌握后安排 D+2、D+7、D+21。"}</section>
  </article>`;
  bindWorkspace(module);
  document.getElementById("workspace").scrollIntoView({behavior: "smooth"});
}

function formPayload(form) {
  return Object.fromEntries(new FormData(form).entries());
}

async function submitActivity(event, module) {
  event.preventDefault();
  const form = event.currentTarget;
  const output = form.querySelector("output");
  output.textContent = "正在检查……";
  try {
    const action = form.dataset.action;
    let result;
    if (action === "answers") result = await submitAnswers(form, module);
    if (action === "evidence") result = await submitEvidence(form, module);
    output.textContent = result;
    await refreshProgress(false);
  } catch (error) {
    output.textContent = error.message;
    output.classList.add("error");
  }
}

async function submitAnswers(form, module) {
  const kind = form.dataset.kind;
  if (kind.startsWith("review-")) {
    const day = Number(kind.split("-")[1]);
    const response = await api("/api/review", {method: "POST", body: JSON.stringify({module_id: module.id, day, answers: formPayload(form)})});
    return `复习得分 ${Math.round(response.result.score * 100)}%`;
  }
  const response = await api("/api/answers", {method: "POST", body: JSON.stringify({module_id: module.id, kind, answers: formPayload(form)})});
  const note = kind === "diagnostic" ? diagnosticGuidance(response.result.score) : "";
  return `得分 ${Math.round(response.result.score * 100)}%${note}`;
}

function diagnosticGuidance(score) {
  if (score < 0.6) return "；建议逐个详细学习全部微课，诊断不替代实验";
  if (score < 0.85) return "；建议重点学习错题相关单元，诊断不替代实验";
  return "；可快速复述微课，但实验、证据和口试仍须完成";
}

async function submitEvidence(form, module) {
  await api("/api/evidence", {method: "POST", body: JSON.stringify({module_id: module.id, response: formPayload(form).response})});
  return "证据说明已保存；AI 口试会继续检查其准确性。";
}

function bindStandardViews(module) {
  const workspace = document.getElementById("module-workspace");
  workspace.querySelectorAll("details[data-standard-view]").forEach((detail) => {
    detail.addEventListener("toggle", async () => {
      if (!detail.open || detail.dataset.viewed === "true") return;
      try {
        const response = await api("/api/standard-view", {method: "POST", body: JSON.stringify({
          module_id: module.id, view_id: detail.dataset.standardView,
        })});
        detail.dataset.viewed = "true";
        detail.querySelector(".view-status").textContent = "已查看";
        app.progress.statuses[module.id] = response.status;
        const count = Object.keys(response.status.standard_views || {}).length;
        const countNode = workspace.querySelector("[data-standard-count]");
        if (countNode) countNode.textContent = `${count}/${module.standard_views.length} 已查看`;
      } catch (error) {
        detail.querySelector(".view-status").textContent = `记录失败：${error.message}`;
      }
    });
  });
}

async function refreshProgress(rerenderWorkspace = true) {
  app.progress = await api("/api/progress");
  renderToday();
  renderModuleGrid();
  if (rerenderWorkspace && app.selectedModule) selectModule(app.selectedModule);
}

async function start() {
  const connection = document.getElementById("connection-status");
  try {
    [app.course, app.progress] = await Promise.all([api("/api/course"), api("/api/progress")]);
    connection.textContent = "已连接本地课程服务；进度写入 build/learning/。";
    renderToday();
    renderModuleGrid();
  } catch (error) {
    connection.textContent = `连接失败：${error.message}`;
    connection.classList.add("error");
  }
}

document.addEventListener("DOMContentLoaded", start);
