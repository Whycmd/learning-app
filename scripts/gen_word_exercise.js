// 生成习题 Word 文档（被 C++ 后端 spawn 调用）
// 用法: node gen_word_exercise.js <exerciseId> <jsonPath> <outDir>
// 习题不入库、不缓存、不复用，仅实时生成
const fs = require('fs');
const path = require('path');
const { Document, Packer, Paragraph, TextRun, HeadingLevel, AlignmentType, PageBreak, BorderStyle } = require('docx');

const [, , exIdArg, jsonPath, outDir] = process.argv;
if (!exIdArg || !jsonPath || !outDir) {
  console.error('usage: gen_word_exercise.js <exerciseId> <jsonPath> <outDir>');
  process.exit(1);
}
const exId = exIdArg;
let data;
try { data = JSON.parse(fs.readFileSync(jsonPath, 'utf8')); }
catch (e) { console.error('read json failed:', e.message); process.exit(2); }

// ---- 排版辅助 ----
const COLOR_TITLE = '1F4E79';
const COLOR_H2 = '2E74B5';
const COLOR_ACCENT = 'C00000';
const COLOR_MUTED = '666666';

function section(arr) { return { children: arr }; }
function empty(s) { return new Paragraph({ spacing: { after: s || 120 } }); }
function pb() { return new Paragraph({ children: [new PageBreak()] }); }

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
    spacing: { before: 240, after: 100 },
    border: { bottom: { style: BorderStyle.SINGLE, size: 2, color: COLOR_H2 } },
    children: [new TextRun({ text, bold: true, size: 26, color: COLOR_H2 })]
  });
}

function p(text, opts = {}) {
  return new Paragraph({
    spacing: { after: opts.after || 60 },
    indent: opts.indent ? { left: opts.indent } : undefined,
    children: [new TextRun({ text: text || '', size: opts.size || 21, bold: opts.bold, italics: opts.italics, color: opts.color })]
  });
}

function rich(runs, opts = {}) {
  return new Paragraph({
    spacing: { after: opts.after || 60 },
    indent: opts.indent ? { left: opts.indent } : undefined,
    children: runs
  });
}

// 题型标签
const typeLabels = {
  word_match: '一、词义匹配',
  word_form_fill: '二、词性转换填空',
  context_blank_fill: '三、语境选词填空',
  cn_to_en_translation: '四、句子翻译（中译英）',
  cloze_choice: '五、词汇辨析完形选择',
  sentence_imitation: '六、例句仿写'
};
const typeOrder = ['word_match', 'word_form_fill', 'context_blank_fill', 'cn_to_en_translation', 'cloze_choice', 'sentence_imitation'];

// 按题型分组
const exercises = data.exercises || [];
const grouped = {};
for (const ex of exercises) {
  const t = ex.type || 'misc';
  (grouped[t] = grouped[t] || []).push(ex);
}

// ---- 构建文档 ----
const children = [];
const kps = data.knowledge_points || [];
const totalWords = kps.length;

// 标题
children.push(titleLine(`高频高阶单词专项测试（${totalWords}词全套真题式试题）`));

// 会读/会写分类
const readWords = kps.filter(k => k.master_require === '会读').map(k => k.standard_content || k.user_input_content);
const writeWords = kps.filter(k => k.master_require === '会写').map(k => k.standard_content || k.user_input_content);
if (readWords.length) {
  children.push(rich([
    new TextRun({ text: '需要会读单词：', bold: true, size: 22, color: COLOR_ACCENT }),
    new TextRun({ text: readWords.join(', '), size: 22 }),
  ]));
}
if (writeWords.length) {
  children.push(rich([
    new TextRun({ text: '需要会写单词：', bold: true, size: 22, color: COLOR_ACCENT }),
    new TextRun({ text: writeWords.join(', '), size: 22 }),
  ]));
}
children.push(empty(200));

// 按题型顺序输出
let qNum = 0;
for (const type of typeOrder) {
  const items = grouped[type];
  if (!items || items.length === 0) continue;

  children.push(sectionHeading(typeLabels[type] || type));

  for (const ex of items) {
    qNum++;

    // 词义匹配 / 完形选择：先显示阅读短文
    if ((type === 'word_match' || type === 'cloze_choice') && ex.passage) {
      children.push(p(ex.passage, { indent: 240, italics: true, color: COLOR_MUTED, after: 100 }));
    }

    // 题目
    if (type === 'word_match') {
      children.push(rich([
        new TextRun({ text: `${qNum}. `, bold: true, size: 22, color: COLOR_ACCENT }),
        new TextRun({ text: ex.question || '', size: 22 }),
      ]));
    } else if (type === 'word_form_fill' || type === 'context_blank_fill') {
      children.push(rich([
        new TextRun({ text: `${qNum}. `, bold: true, size: 22, color: COLOR_ACCENT }),
        new TextRun({ text: ex.question || ex.passage || '', size: 22 }),
      ]));
      if (ex.blanks && ex.blanks.length) {
        children.push(p(`填空数: ${ex.blanks.length}`, { indent: 360, color: COLOR_MUTED }));
      }
    } else if (type === 'cn_to_en_translation') {
      children.push(rich([
        new TextRun({ text: `${qNum}. 【中译英】`, bold: true, size: 22, color: COLOR_ACCENT }),
        new TextRun({ text: ex.prompt || ex.question || '', size: 22 }),
      ]));
    } else if (type === 'cloze_choice') {
      children.push(rich([
        new TextRun({ text: `${qNum}. `, bold: true, size: 22, color: COLOR_ACCENT }),
        new TextRun({ text: ex.question || '', size: 22 }),
      ]));
    } else if (type === 'sentence_imitation') {
      children.push(rich([
        new TextRun({ text: `${qNum}. 范例: `, bold: true, size: 22, color: COLOR_ACCENT }),
        new TextRun({ text: ex.sample || '', size: 22, italics: true }),
      ]));
      if (ex.prompt) children.push(p(ex.prompt, { indent: 360 }));
    }

    // 选项
    if (ex.options && ex.options.length) {
      ex.options.forEach((opt, j) => {
        children.push(p(`${String.fromCharCode(65 + j)}. ${opt}`, { indent: 480 }));
      });
    }
    children.push(empty(60));
  }
}

// 满分词汇总结
children.push(pb());
children.push(sectionHeading('满分词汇总结'));
const memo = data.quick_memo || {};
if (memo.items && memo.items.length) {
  for (const item of memo.items) {
    children.push(rich([
      new TextRun({ text: `${item.knowledge || ''} `, bold: true, size: 21, color: COLOR_H2 }),
      new TextRun({ text: item.brief || '', size: 21, color: COLOR_MUTED }),
    ]));
  }
} else {
  // 从知识点列表生成速记
  for (const kp of kps) {
    const word = kp.standard_content || kp.user_input_content || '';
    const def = (kp.definitions && kp.definitions[0]) ? kp.definitions[0].meaning : '';
    children.push(rich([
      new TextRun({ text: `${word} `, bold: true, size: 21, color: COLOR_H2 }),
      new TextRun({ text: def || '', size: 21, color: COLOR_MUTED }),
    ]));
  }
}

children.push(empty(300));
children.push(new Paragraph({
  alignment: AlignmentType.CENTER,
  children: [new TextRun({ text: '— 习题为智谱AI实时生成，不入库不复用，请完成后及时保存 —', size: 18, color: COLOR_MUTED, italics: true })]
}));

const doc = new Document({ sections: [section(children)] });

(async () => {
  try {
    fs.mkdirSync(outDir, { recursive: true });
    const buf = await Packer.toBuffer(doc);
    const outPath = path.join(outDir, `exercise_${exId}.docx`);
    fs.writeFileSync(outPath, buf);
    console.log(outPath);
    try { fs.unlinkSync(jsonPath); } catch {}
    process.exit(0);
  } catch (e) {
    console.error('docx generation failed:', e.message);
    process.exit(3);
  }
})();
