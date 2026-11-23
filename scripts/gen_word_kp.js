// 生成知识点拓展 Word 文档（被 C++ 后端 spawn 调用）
// 用法:
//   单个: node gen_word_kp.js <kpId> <jsonPath> <outDir>
//   批量: node gen_word_kp.js batch <jsonPath> <outDir>  (JSON 为数组)
//   讲解: node gen_word_kp.js explain <jsonPath> <outDir> (JSON 为单条讲解)
const fs = require('fs');
const path = require('path');
const { Document, Packer, Paragraph, TextRun, HeadingLevel, AlignmentType, PageBreak, Table, TableRow, TableCell, WidthType, BorderStyle, Header, Footer, PageNumber } = require('docx');
const { renderMarkdownBlocks, mergeMarkdownStyles } = require('./md_to_docx');

const [, , modeArg, jsonPath, outDir] = process.argv;
if (!modeArg || !jsonPath || !outDir) {
  console.error('usage: gen_word_kp.js <kpId|batch|explain> <jsonPath> <outDir>');
  process.exit(1);
}

let raw;
try { raw = JSON.parse(fs.readFileSync(jsonPath, 'utf8')); }
catch (e) { console.error('read json failed:', e.message); process.exit(2); }

const isExplain = modeArg === 'explain';
const isBatch = modeArg === 'batch';
const kps = isExplain ? [raw] : (isBatch ? (Array.isArray(raw) ? raw : [raw]) : [raw]);

// ---- 版式常量（D17：A4 单栏、右侧批注留白、宋体 + Times New Roman、小四 1.5 倍行距） ----
const PAGE_SIZE = { width: 11906, height: 16838 }; // A4 (twips)
const PAGE_MARGIN = { top: 1134, bottom: 1134, left: 1417, right: 2268 }; // 右侧留批注空白
const BODY_FONT = { ascii: 'Times New Roman', eastAsia: '宋体', hAnsi: 'Times New Roman' };
const BODY_SIZE = 24;      // 小四
const BODY_LINE = 360;     // 1.5 倍行距

function todayStr() {
  const d = new Date();
  const p = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

function materialTitle() {
  if (isExplain) return 'AI 精讲讲义';
  return isBatch && kps.length > 1 ? `英语精讲讲义（共 ${kps.length} 个词条）` : '英语精讲讲义';
}

// 页眉：材料标题 + 生成日期；页脚：第 X 页 / 共 Y 页
function headerFooter() {
  const head = new Paragraph({
    alignment: AlignmentType.CENTER,
    children: [new TextRun({ text: `${materialTitle()}  ·  ${todayStr()}`, size: 18, color: '666666' })]
  });
  const foot = new Paragraph({
    alignment: AlignmentType.CENTER,
    children: [
      new TextRun({ text: '第 ', size: 18, color: '666666' }),
      new TextRun({ children: [PageNumber.CURRENT], size: 18, color: '666666' }),
      new TextRun({ text: ' 页 / 共 ', size: 18, color: '666666' }),
      new TextRun({ children: [PageNumber.TOTAL_PAGES], size: 18, color: '666666' }),
      new TextRun({ text: ' 页', size: 18, color: '666666' }),
    ]
  });
  return {
    headers: { default: new Header({ children: [head] }) },
    footers: { default: new Footer({ children: [foot] }) }
  };
}

// ---- 排版辅助函数 ----
const COLOR_TITLE = '1F4E79';   // 深蓝
const COLOR_H2 = '2E74B5';      // 中蓝
const COLOR_ACCENT = 'C00000';  // 红色强调
const COLOR_MUTED = '666666';   // 灰色

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

// 正文段落（D17：小四 + 1.5 倍行距）
function p(text, opts = {}) {
  return new Paragraph({
    spacing: { after: opts.after || 60, line: BODY_LINE },
    indent: opts.indent ? { left: opts.indent } : undefined,
    children: [new TextRun({ text: text || '', size: opts.size || BODY_SIZE, bold: opts.bold, italics: opts.italics, color: opts.color })]
  });
}

// 带多格式的段落
function rich(runs, opts = {}) {
  return new Paragraph({
    spacing: { after: opts.after || 60, line: BODY_LINE },
    indent: opts.indent ? { left: opts.indent } : undefined,
    children: runs
  });
}

// ---- v1.5.0-T009 D06：自测题（本地素材，主观为主；答案独立页） ----
const SELF_TEST_LABEL = {
  meaning: '释义', collocation: '搭配', synonym: '同义词', antonym: '反义词', example: '例句'
};

function selfTestsOf(kp) {
  return Array.isArray(kp.self_tests) ? kp.self_tests : [];
}

function selfTestBlock(kp) {
  const tests = selfTestsOf(kp);
  if (!tests.length) {
    return [sectionHeading('自测题'), p('暂无自测题', { color: COLOR_MUTED })];
  }
  const out = [sectionHeading(`自测题（共 ${tests.length} 道）`)];
  tests.forEach((t, i) => {
    out.push(p(`${i + 1}. ${t.prompt || ''}`, { after: 40 }));
    out.push(p('答：____________________________________________', { color: COLOR_MUTED, after: 100 }));
  });
  return out;
}

function answerPage(kpsArr) {
  const blocks = [];
  kpsArr.forEach((kp, idx) => {
    const tests = selfTestsOf(kp);
    if (!tests.length) return;
    const word = kp.standard_content || kp.user_input_content || '';
    blocks.push(sectionHeading(`${idx + 1}. ${word}`));
    tests.forEach((t, i) => {
      const label = SELF_TEST_LABEL[t.type] || '练习';
      const ans = String(t.answer || '').split('\n').filter(Boolean);
      blocks.push(rich([
        new TextRun({ text: `${i + 1}. [${label}] `, bold: true, size: BODY_SIZE, color: COLOR_ACCENT }),
        new TextRun({ text: ans[0] || '', size: BODY_SIZE }),
      ]));
      ans.slice(1).forEach(line => blocks.push(p(line, { indent: 360, color: COLOR_MUTED })));
    });
  });
  return blocks;
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
async function buildExplainChildren(exp) {
  const children = [];
  const converters = [];
  children.push(titleLine('AI 精讲讲义'));
  children.push(new Paragraph({
    spacing: { after: 120, line: BODY_LINE },
    children: [new TextRun({ text: exp.input_text || '', bold: true, size: 32, color: COLOR_TITLE })]
  }));
  if (exp.created_at) {
    children.push(p(`生成日期：${exp.created_at}`, { color: COLOR_MUTED, after: 160 }));
  }
  const md = exp.content_md || '';
  if (String(md).trim()) {
    const rendered = await renderMarkdownBlocks(md);
    if (rendered.blocks.length) children.push(...rendered.blocks);
    if (rendered.converter) converters.push(rendered.converter);
  } else {
    children.push(p('（无正文）'));
  }
  // D06：游客讲解无结构化子表，自测题固定为空
  children.push(sectionHeading('自测题'));
  children.push(p('暂无自测题', { color: COLOR_MUTED }));
  return { children, converters };
}

async function buildChildren(kps) {
  const children = [];
  const converters = [];
  children.push(titleLine('英语学习知识点拓展'));
  children.push(p(`共 ${kps.length} 个知识点  ·  生成时间: ${new Date().toLocaleString('zh-CN')}`, { color: COLOR_MUTED, after: 200 }));

  let hasTests = false;
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

    // v1.5.0-T009：每词一页内附自测题（0 道显示「暂无自测题」）
    if (selfTestsOf(kp).length) hasTests = true;
    children.push(...selfTestBlock(kp));
  }

  // D17：自测题答案独立成页
  if (hasTests) {
    children.push(pageBreak());
    children.push(titleLine('自测题参考答案'));
    children.push(...answerPage(kps));
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
    const { children, converters } = isExplain
      ? await buildExplainChildren(raw)
      : await buildChildren(kps);
    const baseStyles = {
      default: {
        document: {
          run: { font: BODY_FONT, size: BODY_SIZE },
          paragraph: { spacing: { line: BODY_LINE } }
        }
      }
    };
    const options = {
      styles: baseStyles,
      sections: [{
        properties: { page: { size: PAGE_SIZE, margin: PAGE_MARGIN } },
        ...headerFooter(),
        children
      }]
    };
    if (converters.length) {
      const styles = mergeMarkdownStyles(converters);
      if (styles.numbering) options.numbering = styles.numbering;
      options.styles = {
        ...baseStyles,
        paragraphStyles: styles.paragraphStyles,
        characterStyles: styles.characterStyles,
      };
    }
    const doc = new Document(options);
    const buf = await Packer.toBuffer(doc);
    const outName = isExplain ? `explanation_${raw.id}.docx`
      : (isBatch ? 'knowledge_points_batch.docx' : `knowledge_point_${modeArg}.docx`);
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
