// 生成统一试卷 Word 文档（被 C++ 后端 spawn 调用）
// 用法: node gen_word_exercise.js <idArg> <jsonPath> <outDir>
// 数据: { kind: 'review'|'custom', id?, title?, exercises:[...], knowledge_points?, quick_memo?,
//         spec?, difficulty?, style? }
// 复习卷输出 exercise_<idArg>.docx；定制练习输出 custom_exercise_<id>.docx
// 统一模板：A4 单栏、六类题型固定顺序、题号连续、答案与解析另页、空白卷
const fs = require('fs');
const path = require('path');
const { Document, Packer, Paragraph, TextRun, HeadingLevel, AlignmentType, PageBreak, BorderStyle, Header, Footer, PageNumber } = require('docx');

const [, , idArg, jsonPath, outDir] = process.argv;
if (!idArg || !jsonPath || !outDir) {
  console.error('usage: gen_word_exercise.js <idArg> <jsonPath> <outDir>');
  process.exit(1);
}
let data;
try { data = JSON.parse(fs.readFileSync(jsonPath, 'utf8')); }
catch (e) { console.error('read json failed:', e.message); process.exit(2); }

// ---- 版式常量（D17） ----
const PAGE_SIZE = { width: 11906, height: 16838 }; // A4
const PAGE_MARGIN = { top: 1134, bottom: 1134, left: 1417, right: 1417 };
const BODY_FONT = { ascii: 'Times New Roman', eastAsia: '宋体', hAnsi: 'Times New Roman' };
const BODY_SIZE = 24;
const BODY_LINE = 360;

const COLOR_TITLE = '1F4E79';
const COLOR_H2 = '2E74B5';
const COLOR_ACCENT = 'C00000';
const COLOR_MUTED = '666666';

const TYPE_ORDER = ['word_match', 'word_form_fill', 'context_blank_fill',
  'cn_to_en_translation', 'cloze_choice', 'sentence_imitation', 'long_reading_cloze', 'full_cloze'];
const TYPE_LABELS = {
  word_match: '一、词义匹配',
  word_form_fill: '二、词性转换填空',
  context_blank_fill: '三、语境选词填空',
  cn_to_en_translation: '四、句子翻译（中译英）',
  cloze_choice: '五、词汇辨析完形选择',
  sentence_imitation: '六、例句仿写',
  long_reading_cloze: '七、长篇阅读语法填空',
  full_cloze: '八、完整完形填空'
};

const isCustom = data.kind === 'custom';
const title = data.title || (isCustom ? '定制练习卷' : '高频高阶单词专项测试');

function todayStr() {
  const d = new Date();
  const p = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

function titleLine(text) {
  return new Paragraph({
    heading: HeadingLevel.TITLE,
    alignment: AlignmentType.CENTER,
    spacing: { after: 200 },
    children: [new TextRun({ text, bold: true, size: 36, color: COLOR_TITLE })]
  });
}
function sectionHeading(text) {
  return new Paragraph({
    spacing: { before: 240, after: 100, line: BODY_LINE },
    border: { bottom: { style: BorderStyle.SINGLE, size: 2, color: COLOR_H2 } },
    children: [new TextRun({ text, bold: true, size: 26, color: COLOR_H2 })]
  });
}
function p(text, opts = {}) {
  return new Paragraph({
    spacing: { after: opts.after || 60, line: BODY_LINE },
    indent: opts.indent ? { left: opts.indent } : undefined,
    children: [new TextRun({ text: text || '', size: opts.size || BODY_SIZE, bold: opts.bold, italics: opts.italics, color: opts.color })]
  });
}
function rich(runs, opts = {}) {
  return new Paragraph({
    spacing: { after: opts.after || 60, line: BODY_LINE },
    indent: opts.indent ? { left: opts.indent } : undefined,
    children: runs
  });
}
function empty(n) { return new Paragraph({ spacing: { after: n || 120 } }); }
function pb() { return new Paragraph({ children: [new PageBreak()] }); }

// 主观题留白 3 行、填空 1 行、选择/匹配不留白
function answerLines(n) {
  const out = [];
  for (let i = 0; i < n; i++) {
    out.push(p('________________________________________________', { color: '999999', indent: 480, after: 120 }));
  }
  return out;
}

function optionText(opt, idx) {
  const s = String(opt || '').replace(/^\s*[A-Da-d]\s*[.、):：]\s*/, '')
    .replace(/^\s*[Ａ-Ｄ]\s*[.．、)]\s*/, '');
  return `${String.fromCharCode(65 + idx)}. ${s}`;
}

// 题目正文取 review(question) 或 custom(stem/passage)
// 不再用目标词兜底生成题干，避免导出卷直接泄题
function stemOf(ex) {
  return ex.stem || ex.question || ex.passage || '';
}
// 旧数据兼容：word 字段偶尔存的是完整题干/中文提示，而不是单个目标词。
// 只有明显不是单个词的内容才作为题干回退，避免把目标词直接印进试卷。
function legacyStemFromWord(ex) {
  const w = String(ex.word || '').trim();
  if (!w) return '';
  return (w.length > 20 || /\s/.test(w) || /[\u4e00-\u9fff]/.test(w)) ? w : '';
}
function promptOf(ex) {
  const legacy = legacyStemFromWord(ex);
  if (ex.type === 'word_match') {
    return ex.stem || ex.question || legacy || '阅读短文，完成下列题目。';
  }
  if (ex.type === 'context_blank_fill') return ex.stem || ex.question || legacy || '根据语境填入合适的词。';
  if (ex.type === 'cn_to_en_translation') return ex.prompt || ex.stem || ex.question || legacy || '翻译下列句子。';
  return stemOf(ex);
}
// 空位：单空只用 ______；多空保留编号 “______ (n)”
function renderBlankPassage(text, numbered) {
  const s = String(text || '');
  const n = (s.match(/\[\[\s*\d+\s*\]\]/g) || []).length;
  if (numbered || n > 1) return s.replace(/\[\[\s*(\d+)\s*\]\]/g, '______ ($1)');
  return s.replace(/\[\[\s*\d+\s*\]\]/g, '______');
}

// 词形填空：基础词紧跟空位，不单独列出
function renderFormStem(ex) {
  let s = String(ex.stem || ex.question || '').replace(/\[\[\s*\d+\s*\]\]/g, '______')
    .replace(/_{3,}/g, '______');
  const w = String(ex.word || '').trim();
  if (w && s.indexOf('(' + w + ')') < 0 && s.indexOf('______') >= 0)
    s = s.replace('______', '______ (' + w + ')');
  return s;
}

function metaLine() {
  if (!isCustom) return '';
  const spec = data.spec || {};
  const diffMap = { basic: '基础', advanced: '进阶', challenge: '挑战' };
  const styleMap = { exam: '应试', situational: '情景', story: '故事', academic: '学术' };
  let src = '主题自选';
  const s = spec.source || {};
  if (s.type === 'theme') src = '主题：' + (s.theme || '通用');
  else if (s.type === 'wordlist' || s.type === 'mylist') src = '词表取词';
  else if (s.type === 'range') src = '教材范围';
  else if (s.type === 'studying') src = '在学词';
  else if (s.type === 'wrong') src = '错题';
  return `取词依据：${src}　难度：${diffMap[data.difficulty] || data.difficulty || '—'}　风格：${styleMap[data.style] || data.style || '—'}`;
}

// ---- 构建内容 ----
const children = [];
const answers = [];   // { no, answer, explanation }
const exercises = Array.isArray(data.exercises) ? data.exercises
  : (Array.isArray(data.items) ? data.items : []);
const grouped = {};
for (const ex of exercises) {
  const t = ex.type || '';
  if (!TYPE_ORDER.includes(t)) continue;
  (grouped[t] = grouped[t] || []).push(ex);
}

children.push(titleLine(title));
children.push(p(`生成日期：${todayStr()}`, { color: COLOR_MUTED, after: 100 }));

if (isCustom) {
  const meta = metaLine();
  if (meta) children.push(p(meta, { color: COLOR_MUTED, after: 100 }));
} else {
  const kps = Array.isArray(data.knowledge_points) ? data.knowledge_points : [];
  const readWords = kps.filter(k => k.master_require === '会读').map(k => k.standard_content || k.user_input_content);
  const writeWords = kps.filter(k => k.master_require === '会写').map(k => k.standard_content || k.user_input_content);
  if (readWords.length) children.push(rich([
    new TextRun({ text: '需要会读单词：', bold: true, color: COLOR_ACCENT }),
    new TextRun({ text: readWords.join(', ') }),
  ]));
  if (writeWords.length) children.push(rich([
    new TextRun({ text: '需要会写单词：', bold: true, color: COLOR_ACCENT }),
    new TextRun({ text: writeWords.join(', ') }),
  ]));
  children.push(empty(160));
}

let qNum = 0;
for (const type of TYPE_ORDER) {
  const items = grouped[type];
  if (!items || !items.length) continue;
  children.push(sectionHeading(TYPE_LABELS[type]));

  // 词性转换填空：基础词已内联在空位后，不再单独列候选词

  for (const ex of items) {
    qNum++;

    // 完整完形填空：短文 + 逐空选择题，答案在答案页按空列出
    if (type === 'full_cloze') {
      children.push(rich([
        new TextRun({ text: `${qNum}. `, bold: true, color: COLOR_ACCENT }),
        new TextRun({ text: ex.stem || '阅读下面短文，从每题所给的 A、B、C、D 四个选项中选出最佳选项。' }),
      ]));
      children.push(p(renderBlankPassage(ex.passage, true), { indent: 240, italics: true, color: COLOR_MUTED, after: 100 }));
      const qs = Array.isArray(ex.questions) ? ex.questions : [];
      qs.forEach(item => {
        children.push(p(`(${item.no || ''})`, { bold: true, after: 20 }));
        (item.options || []).forEach((opt, j) => children.push(p(optionText(opt, j), { indent: 480, after: 20 })));
      });
      answers.push({
        no: qNum,
        answer: qs.map(x => `(${x.no || ''}) ${x.answer || ''}`).join('  ') || ex.answer || '',
        explanation: qs.map(x => x.explanation || '').filter(Boolean).join('；') || ex.explanation || '',
      });
      continue;
    }

    // 长篇阅读语法填空：短文 + 编号空位，答案在答案页按空列出
    if (type === 'long_reading_cloze') {
      children.push(rich([
        new TextRun({ text: `${qNum}. `, bold: true, color: COLOR_ACCENT }),
        new TextRun({ text: ex.stem || '根据短文内容，在空白处填入适当的词或所给词的正确形式。' }),
      ]));
      children.push(p(renderBlankPassage(ex.passage, true), { indent: 240, italics: true, color: COLOR_MUTED, after: 100 }));
      const blanks = Array.isArray(ex.blanks) ? ex.blanks : [];
      const ansText = blanks.map(b => `(${b.no || ''}) ${b.answer || ''}`).join('  ');
      const expText = blanks.map(b => b.explanation || '').filter(Boolean).join('；');
      answers.push({
        no: qNum,
        answer: ansText || ex.answer || '',
        explanation: expText || ex.explanation || '',
      });
      continue;
    }

    let stem = type === 'word_form_fill' ? renderFormStem(ex) : promptOf(ex);
    if (type === 'word_match' || type === 'cloze_choice' || type === 'context_blank_fill') {
      stem = renderBlankPassage(stem, false);
    }

    // 阅读短文（词义匹配 / 完形选择）
    if ((type === 'word_match' || type === 'cloze_choice') && ex.passage) {
      children.push(p(renderBlankPassage(ex.passage, false), { indent: 240, italics: true, color: COLOR_MUTED, after: 100 }));
    }

    if (type === 'cn_to_en_translation') {
      children.push(rich([
        new TextRun({ text: `${qNum}. 【中译英】`, bold: true, color: COLOR_ACCENT }),
        new TextRun({ text: ex.prompt || stem }),
      ]));
    } else if (type === 'sentence_imitation') {
      children.push(rich([
        new TextRun({ text: `${qNum}. 范例：`, bold: true, color: COLOR_ACCENT }),
        new TextRun({ text: ex.sample || '', italics: true }),
      ]));
      if (ex.prompt) children.push(p(ex.prompt, { indent: 360 }));
    } else {
      children.push(rich([
        new TextRun({ text: `${qNum}. `, bold: true, color: COLOR_ACCENT }),
        new TextRun({ text: stem }),
      ]));
    }

    // 选项
    if (Array.isArray(ex.options) && ex.options.length) {
      ex.options.forEach((opt, j) => children.push(p(optionText(opt, j), { indent: 480 })));
    }

    // 留白：主观 3 行、填空 1 行
    if (type === 'cn_to_en_translation' || type === 'sentence_imitation') {
      children.push(...answerLines(3));
    } else if (type === 'word_form_fill' || type === 'context_blank_fill') {
      children.push(...answerLines(1));
    } else {
      children.push(empty(60));
    }

    answers.push({
      no: qNum,
      answer: ex.answer || ex.reference_answer || (ex.sample ? '（参考范例见题目）' : ''),
      explanation: ex.explanation || ex.error_analysis || '',
    });
  }
}

// 满分词汇总结（复习卷；D19：保留在答案页之前）
if (!isCustom) {
  const memo = data.quick_memo || {};
  const kps = Array.isArray(data.knowledge_points) ? data.knowledge_points : [];
  children.push(pb());
  children.push(sectionHeading('满分词汇总结'));
  if (Array.isArray(memo.items) && memo.items.length) {
    for (const item of memo.items) {
      children.push(rich([
        new TextRun({ text: `${item.knowledge || ''} `, bold: true, color: COLOR_H2 }),
        new TextRun({ text: item.brief || '', color: COLOR_MUTED }),
      ]));
    }
  } else if (kps.length) {
    for (const kp of kps) {
      const word = kp.standard_content || kp.user_input_content || '';
      const def = (kp.definitions && kp.definitions[0]) ? kp.definitions[0].meaning : '';
      children.push(rich([
        new TextRun({ text: `${word} `, bold: true, color: COLOR_H2 }),
        new TextRun({ text: def || '', color: COLOR_MUTED }),
      ]));
    }
  } else {
    children.push(p('（无）', { color: COLOR_MUTED }));
  }
}

// 答案与解析独立页
children.push(pb());
children.push(sectionHeading('参考答案与解析'));
if (!answers.length) {
  children.push(p('（无）', { color: COLOR_MUTED }));
}
for (const a of answers) {
  children.push(rich([
    new TextRun({ text: `${a.no}. `, bold: true, color: COLOR_ACCENT }),
    new TextRun({ text: a.answer || '（略）' }),
  ]));
  if (a.explanation) children.push(p(a.explanation, { indent: 360, color: COLOR_MUTED }));
}

children.push(empty(300));
children.push(new Paragraph({
  alignment: AlignmentType.CENTER,
  children: [new TextRun({ text: isCustom ? '— 定制练习由 AI 生成，答案与解析仅供自主核对 —' : '— 试卷为按需导出，答案与解析仅供自主核对 —', size: 18, color: COLOR_MUTED, italics: true })]
}));

// 页眉：材料标题 + 生成日期；页脚：第 X 页 / 共 Y 页
const header = new Paragraph({
  alignment: AlignmentType.CENTER,
  children: [new TextRun({ text: `${title}  ·  ${todayStr()}`, size: 18, color: COLOR_MUTED })]
});
const footer = new Paragraph({
  alignment: AlignmentType.CENTER,
  children: [
    new TextRun({ text: '第 ', size: 18, color: COLOR_MUTED }),
    new TextRun({ children: [PageNumber.CURRENT], size: 18, color: COLOR_MUTED }),
    new TextRun({ text: ' 页 / 共 ', size: 18, color: COLOR_MUTED }),
    new TextRun({ children: [PageNumber.TOTAL_PAGES], size: 18, color: COLOR_MUTED }),
    new TextRun({ text: ' 页', size: 18, color: COLOR_MUTED }),
  ]
});

const doc = new Document({
  styles: {
    default: {
      document: {
        run: { font: BODY_FONT, size: BODY_SIZE },
        paragraph: { spacing: { line: BODY_LINE } }
      }
    }
  },
  sections: [{
    properties: { page: { size: PAGE_SIZE, margin: PAGE_MARGIN } },
    headers: { default: new Header({ children: [header] }) },
    footers: { default: new Footer({ children: [footer] }) },
    children
  }]
});

(async () => {
  try {
    fs.mkdirSync(outDir, { recursive: true });
    const buf = await Packer.toBuffer(doc);
    const outName = isCustom
      ? `custom_exercise_${data.id || idArg}.docx`
      : `exercise_${idArg}.docx`;
    const outPath = path.join(outDir, outName);
    fs.writeFileSync(outPath, buf);
    console.log(outPath);
    try { fs.unlinkSync(jsonPath); } catch {}
    process.exit(0);
  } catch (e) {
    console.error('docx generation failed:', e.message);
    process.exit(3);
  }
})();
