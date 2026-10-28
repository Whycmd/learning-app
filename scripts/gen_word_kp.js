// 生成知识点拓展 Word 文档（被 C++ 后端 spawn 调用）
// 用法:
//   单个: node gen_word_kp.js <kpId> <jsonPath> <outDir>
//   批量: node gen_word_kp.js batch <jsonPath> <outDir>  (JSON 为数组)
const fs = require('fs');
const path = require('path');
const { Document, Packer, Paragraph, TextRun, HeadingLevel, AlignmentType, PageBreak, Table, TableRow, TableCell, WidthType, BorderStyle } = require('docx');
const { renderMarkdownBlocks, mergeMarkdownStyles } = require('./md_to_docx');

const [, , modeArg, jsonPath, outDir] = process.argv;
if (!modeArg || !jsonPath || !outDir) {
  console.error('usage: gen_word_kp.js <kpId|batch> <jsonPath> <outDir>');
  process.exit(1);
}

let raw;
try { raw = JSON.parse(fs.readFileSync(jsonPath, 'utf8')); }
catch (e) { console.error('read json failed:', e.message); process.exit(2); }

const isBatch = modeArg === 'batch';
const kps = isBatch ? (Array.isArray(raw) ? raw : [raw]) : [raw];

// ---- 排版辅助函数 ----
const COLOR_TITLE = '1F4E79';   // 深蓝
const COLOR_H2 = '2E74B5';      // 中蓝
const COLOR_ACCENT = 'C00000';  // 红色强调
const COLOR_MUTED = '666666';   // 灰色

function section(arr) { return { children: arr }; }
function empty(size) { return new Paragraph({ spacing: { after: size || 120 } }); }
function pageBreak() { return new Paragraph({ children: [new PageBreak()] }); }

// 主标题
function titleLine(text) {
  return new Paragraph({
    heading: HeadingLevel.TITLE,
    alignment: AlignmentType.CENTER,
    spacing: { after: 200 },
    children: [new TextRun({ text, bold: true, size: 36, color: COLOR_TITLE })]
  });
}

// 知识点编号 + 单词 + 音标
function kpHeader(idx, kp) {
  const word = kp.standard_content || kp.user_input_content || '';
  const extras = kp.extras || [];
  const phonetic = (extras.find(e => e.extra_type === 'phonetic') || {}).content || '';
  return new Paragraph({
    spacing: { before: 300, after: 100 },
    border: { bottom: { style: BorderStyle.SINGLE, size: 3, color: COLOR_H2 } },
    children: [
      new TextRun({ text: `${idx}. `, bold: true, size: 28, color: COLOR_ACCENT }),
      new TextRun({ text: word, bold: true, size: 32, color: COLOR_TITLE }),
      ...(phonetic ? [new TextRun({ text: `  ${phonetic}`, size: 24, color: COLOR_MUTED, italics: true })] : []),
    ]
  });
}

// 板块标题（如"核心释义 & 真题例句"）
function sectionHeading(text) {
  return new Paragraph({
    spacing: { before: 180, after: 80 },
    children: [new TextRun({ text, bold: true, size: 24, color: COLOR_H2 })]
  });
}

// 正文段落
function p(text, opts = {}) {
  return new Paragraph({
    spacing: { after: opts.after || 60 },
    indent: opts.indent ? { left: opts.indent } : undefined,
    children: [new TextRun({ text: text || '', size: opts.size || 21, bold: opts.bold, italics: opts.italics, color: opts.color })]
  });
}

// 带多格式的段落
function rich(runs, opts = {}) {
  return new Paragraph({
    spacing: { after: opts.after || 60 },
    indent: opts.indent ? { left: opts.indent } : undefined,
    children: runs
  });
}

// 获取 extra 字段内容
function getExtra(kp, type) {
  const extras = kp.extras || [];
  const found = extras.find(e => e.extra_type === type);
  return found ? found.content : '';
}

// ---- 旧格式：音标/词根/搭配/同义/补充等逐条 extras ----
function appendLegacyBody(children, kp) {
  // 使用频率（页眉下简要一行）
  const freq = getExtra(kp, 'frequency');
  if (freq) {
    children.push(p(freq, { color: COLOR_MUTED, after: 80 }));
  }

  // 一、核心含义与例句（每个含义紧跟例句）
  children.push(sectionHeading('一、核心含义与例句'));
  const defs = kp.definitions || [];
  if (defs.length) {
    defs.forEach((d, j) => {
      children.push(rich([
        new TextRun({ text: `${j+1}. [${d.pos || ''}] `, bold: true, size: 21, color: COLOR_ACCENT }),
        new TextRun({ text: d.meaning || '', size: 21 }),
      ]));
      if (d.example) {
        children.push(p(d.example, { indent: 360, italics: true, color: COLOR_MUTED }));
      }
    });
  } else {
    children.push(p('（无）'));
  }

  // 二、词根与词源
  const root = getExtra(kp, 'root');
  if (root) {
    children.push(sectionHeading('二、词根与词源'));
    String(root).split('\n').filter(Boolean).forEach(line => children.push(p(line)));
  }

  // 三、同源词汇
  const family = getExtra(kp, 'word_family');
  if (family) {
    children.push(sectionHeading('三、同源词汇'));
    String(family).split('\n').filter(Boolean).forEach(line => children.push(p(line)));
  }

  // 四、典故、俗语与固定搭配
  const allusions = getExtra(kp, 'allusions');
  const cols = kp.collocations || [];
  if (allusions || cols.length) {
    children.push(sectionHeading('四、典故、俗语与固定搭配'));
    if (allusions) {
      String(allusions).split('\n').filter(Boolean).forEach(line => children.push(p(line, { after: 80 })));
    }
    cols.forEach((c) => {
      children.push(rich([
        new TextRun({ text: `• ${c.collocation || ''}`, bold: true, size: 21, color: COLOR_H2 }),
      ]));
      if (c.example) {
        children.push(p(c.example, { indent: 360, italics: true, color: COLOR_MUTED }));
      }
    });
  }

  // 五、同义词及辨析
  const syns = kp.synonyms || [];
  const synAnalysis = getExtra(kp, 'synonym_analysis');
  const ants = kp.antonyms || [];
  if (syns.length || synAnalysis || ants.length) {
    children.push(sectionHeading('五、同义词及辨析'));
    if (syns.length) {
      children.push(rich([
        new TextRun({ text: '近义词: ', bold: true, size: 21 }),
        new TextRun({ text: syns.map(s => typeof s === 'string' ? s : s.synonym).join(' / '), size: 21 }),
      ]));
    }
    if (synAnalysis) {
      String(synAnalysis).split('\n').filter(Boolean).forEach(line => children.push(p(line, { after: 80 })));
    }
    if (ants.length) {
      children.push(rich([
        new TextRun({ text: '反义词: ', bold: true, size: 21 }),
        new TextRun({ text: ants.map(s => typeof s === 'string' ? s : s.antonym).join(' / '), size: 21 }),
      ]));
    }
  }

  // 六、补充相关知识
  const phraseAnalysis = getExtra(kp, 'phrase_analysis');
  const keyPoints = getExtra(kp, 'key_points');
  const register = getExtra(kp, 'register');
  const cultural = getExtra(kp, 'cultural');
  const relatedWords = getExtra(kp, 'related_words');
  const extras6 = [
    phraseAnalysis && { h: '短语解析', t: phraseAnalysis },
    keyPoints && { h: '重难点', t: keyPoints },
    register && { h: '语域说明', t: register },
    cultural && { h: '文化延伸', t: cultural },
    relatedWords && { h: '同类词语', t: relatedWords },
  ].filter(Boolean);
  if (extras6.length) {
    children.push(sectionHeading('六、补充相关知识'));
    extras6.forEach(item => {
      if (item.h) children.push(rich([new TextRun({ text: `${item.h}：`, bold: true, size: 21, color: COLOR_TITLE })]));
      String(item.t).split('\n').filter(Boolean).forEach(line => children.push(p(line, { after: 80 })));
    });
  }
}

// ---- 构建文档内容 ----
async function buildChildren(kps) {
  const children = [];
  const converters = [];
  children.push(titleLine('英语学习知识点拓展'));
  children.push(p(`共 ${kps.length} 个知识点  ·  生成时间: ${new Date().toLocaleString('zh-CN')}`, { color: COLOR_MUTED, after: 200 }));

  for (let i = 0; i < kps.length; i++) {
    const kp = kps[i];
    if (i > 0) children.push(pageBreak());

    // 知识点标题 + 音标
    children.push(kpHeader(i + 1, kp));

    // v1.4.0-T010：新格式精讲整块 Markdown；无则保持旧格式
    const markdown = getExtra(kp, 'markdown_content');
    if (markdown && String(markdown).trim()) {
      const rendered = await renderMarkdownBlocks(markdown);
      if (rendered.blocks.length) children.push(...rendered.blocks);
      if (rendered.converter) converters.push(rendered.converter);
    } else {
      appendLegacyBody(children, kp);
    }
  }

  children.push(empty(300));
  children.push(new Paragraph({
    alignment: AlignmentType.CENTER,
    children: [new TextRun({ text: '— 文档由智谱AI GLM-4-Flash 固化生成 · 永久复用 —', size: 18, color: COLOR_MUTED, italics: true })]
  }));

  return { children, converters };
}

(async () => {
  try {
    fs.mkdirSync(outDir, { recursive: true });
    const { children, converters } = await buildChildren(kps);
    const options = { sections: [section(children)] };
    if (converters.length) {
      const styles = mergeMarkdownStyles(converters);
      if (styles.numbering) options.numbering = styles.numbering;
      options.styles = {
        paragraphStyles: styles.paragraphStyles,
        characterStyles: styles.characterStyles,
      };
    }
    const doc = new Document(options);
    const buf = await Packer.toBuffer(doc);
    const outName = isBatch ? 'knowledge_points_batch.docx' : `knowledge_point_${modeArg}.docx`;
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
