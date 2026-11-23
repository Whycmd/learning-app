// 生成单词卡 Word 文档（被 C++ 后端 spawn 调用）
// 用法: node gen_word_wordbook.js <wordbook|wordlist> <idArg> <jsonPath> <outDir>
// 数据: { title?, id?, words:[{word,meaning}] }
// 输出: wordbook_<idArg>.docx / wordlist_<idArg>.docx
// 版式：A4 2×3 单面，仅单词 + 释义，带裁切边框（D09/D19）
const fs = require('fs');
const path = require('path');
const { Document, Packer, Paragraph, TextRun, AlignmentType, PageBreak, Table, TableRow, TableCell, WidthType, BorderStyle, Header, Footer, PageNumber, HeightRule, VerticalAlign, TableLayoutType } = require('docx');

const [, , kindArg, idArg, jsonPath, outDir] = process.argv;
if (!kindArg || !idArg || !jsonPath || !outDir) {
  console.error('usage: gen_word_wordbook.js <wordbook|wordlist> <idArg> <jsonPath> <outDir>');
  process.exit(1);
}
let data;
try { data = JSON.parse(fs.readFileSync(jsonPath, 'utf8')); }
catch (e) { console.error('read json failed:', e.message); process.exit(2); }

const PAGE_SIZE = { width: 11906, height: 16838 }; // A4
const PAGE_MARGIN = { top: 1134, bottom: 1134, left: 1134, right: 1134 };
const BODY_FONT = { ascii: 'Times New Roman', eastAsia: '宋体', hAnsi: 'Times New Roman' };
const COLOR_MUTED = '666666';

// 内容区约 9638 x 14570 twips，2 列 × 3 行
const CELL_W = 4819;
const CELL_H = 4856;

const words = Array.isArray(data.words) ? data.words : [];
const title = data.title || '单词卡';

function todayStr() {
  const d = new Date();
  const p = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

function cardCell(w) {
  const children = w ? [
    new Paragraph({
      alignment: AlignmentType.CENTER,
      spacing: { before: 0, after: 160 },
      children: [new TextRun({ text: w.word || '', bold: true, size: 34, font: BODY_FONT })],
    }),
    new Paragraph({
      alignment: AlignmentType.CENTER,
      children: [new TextRun({ text: w.meaning || '', size: 22, color: '333333', font: BODY_FONT })],
    }),
  ] : [new Paragraph({ children: [] })];
  return new TableCell({
    width: { size: CELL_W, type: WidthType.DXA },
    height: { value: CELL_H, rule: HeightRule.ATLEAST },
    verticalAlign: VerticalAlign.CENTER,
    margins: { top: 180, bottom: 180, left: 180, right: 180 },
    borders: {
      top: { style: BorderStyle.DASHED, size: 6, color: 'AAAAAA' },
      bottom: { style: BorderStyle.DASHED, size: 6, color: 'AAAAAA' },
      left: { style: BorderStyle.DASHED, size: 6, color: 'AAAAAA' },
      right: { style: BorderStyle.DASHED, size: 6, color: 'AAAAAA' },
    },
    children,
  });
}

// 每页 6 张卡（2 列 × 3 行）
const children = [];
for (let start = 0; start < words.length; start += 6) {
  if (start > 0) children.push(new Paragraph({ children: [new PageBreak()] }));
  const chunk = words.slice(start, start + 6);
  const rows = [];
  for (let r = 0; r < 3; r++) {
    rows.push(new TableRow({
      children: [cardCell(chunk[r * 2]), cardCell(chunk[r * 2 + 1])],
      height: { value: CELL_H, rule: HeightRule.ATLEAST },
    }));
  }
  children.push(new Table({
    rows,
    width: { size: CELL_W * 2, type: WidthType.DXA },
    layout: TableLayoutType.FIXED,
  }));
}

const header = new Paragraph({
  alignment: AlignmentType.CENTER,
  children: [new TextRun({ text: `${title}  ·  ${todayStr()}`, size: 18, color: COLOR_MUTED })],
});
const footer = new Paragraph({
  alignment: AlignmentType.CENTER,
  children: [
    new TextRun({ text: '第 ', size: 18, color: COLOR_MUTED }),
    new TextRun({ children: [PageNumber.CURRENT], size: 18, color: COLOR_MUTED }),
    new TextRun({ text: ' 页 / 共 ', size: 18, color: COLOR_MUTED }),
    new TextRun({ children: [PageNumber.TOTAL_PAGES], size: 18, color: COLOR_MUTED }),
    new TextRun({ text: ' 页', size: 18, color: COLOR_MUTED }),
  ],
});

const doc = new Document({
  styles: {
    default: {
      document: {
        run: { font: BODY_FONT, size: 24 },
        paragraph: { spacing: { line: 300 } },
      },
    },
  },
  sections: [{
    properties: { page: { size: PAGE_SIZE, margin: PAGE_MARGIN } },
    headers: { default: new Header({ children: [header] }) },
    footers: { default: new Footer({ children: [footer] }) },
    children,
  }],
});

(async () => {
  try {
    fs.mkdirSync(outDir, { recursive: true });
    const buf = await Packer.toBuffer(doc);
    const prefix = kindArg === 'wordlist' ? 'wordlist' : 'wordbook';
    const outPath = path.join(outDir, `${prefix}_${idArg}.docx`);
    fs.writeFileSync(outPath, buf);
    console.log(outPath);
    try { fs.unlinkSync(jsonPath); } catch {}
    process.exit(0);
  } catch (e) {
    console.error('docx generation failed:', e.message);
    process.exit(3);
  }
})();
