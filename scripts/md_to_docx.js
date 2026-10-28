// Markdown → docx 适配层（v1.4.0-T013）
// [FUNCTIONAL] 以后更换 Markdown 渲染实现时，只需改本文件的 renderMarkdownBlocks / mergeMarkdownStyles。
const { MarkdownDocx } = require('markdown-docx');

// 与 gen_word_kp.js 保持同一套配色（docx 颜色不带 #）
const DEFAULT_THEME = {
  heading1: '1F4E79',
  heading2: '2E74B5',
  heading3: '2E74B5',
  heading4: '1F4E79',
  heading5: '1F4E79',
  heading6: '1F4E79',
  link: '2E74B5',
  code: '1F4E79',
  tag: '666666',
  border: 'D9D9D9',
  codespan: 'C00000',
  codeBackground: 'F2F2F2',
  blockquote: '666666',
  blockquoteBackground: 'F7F7F7',
  del: '666666',
  hr: 'D9D9D9',
  html: '666666',
  tableHeaderBackground: 'DEEAF6',
  heading1Size: 28,
  heading2Size: 24,
  heading3Size: 22,
  heading4Size: 21,
  heading5Size: 21,
  heading6Size: 21,
  spaceSize: 8,
  codeSize: 18,
  linkUnderline: true,
  bodySize: 21,
  bodyFont: 'Calibri',
  headingFont: 'Calibri',
  codeFont: 'Consolas',
  blockquoteFont: 'Calibri',
  collapseEmptyLines: true,
};

// 去掉精讲自带的 H1，避免与 Word 的词条标题重复
function stripDuplicateTitle(markdown) {
  return String(markdown || '')
    .replace(/^\uFEFF/, '')
    .replace(/^(?:[ \t]*\r?\n)*[ \t]*#[ \t]*单词精讲[^\r\n]*(?:\r?\n|$)/, '');
}

// 原始 HTML 虽不解析，仍先移除 script/style 的正文，避免残留可读内容
function stripExecutableHtml(markdown) {
  return String(markdown || '')
    .replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '')
    .replace(/<script\b[^>]*\/>/gi, '')
    .replace(/<style\b[^>]*>[\s\S]*?<\/style>/gi, '');
}

// AI 偶尔使用 \(...\) / \[...\]，统一成 marked 能识别的 $...$
function normalizeLatex(markdown) {
  return String(markdown || '')
    .replace(/\\\[([\s\S]*?)\\\]/g, (_, expr) => `\n$$\n${expr.trim()}\n$$\n`)
    .replace(/\\\(([\s\S]*?)\\\)/g, (_, expr) => `$${expr.trim()}$`);
}

function prepareMarkdown(markdown) {
  return normalizeLatex(stripExecutableHtml(stripDuplicateTitle(markdown)));
}

async function renderMarkdownBlocks(markdown, theme) {
  const md = prepareMarkdown(markdown);
  if (!md.trim()) return { blocks: [], converter: null };

  const converter = new MarkdownDocx(md, {
    gfm: true,
    breaks: false,
    ignoreImage: true,
    ignoreHtml: true,
    ignoreFootnote: true,
    math: {
      engine: 'katex',
      katexOptions: { throwOnError: false, strict: false },
      libreOfficeCompat: true,
    },
    theme: { ...DEFAULT_THEME, ...(theme || {}) },
  });
  const blocks = await converter.toSection();
  return { blocks, converter };
}

// 合并多个转换器的样式与编号，供同一份 Word 文档复用
function mergeMarkdownStyles(converters) {
  const paragraphStyles = new Map();
  const characterStyles = new Map();
  let numbering;

  for (const converter of converters) {
    if (!converter) continue;
    const markdownStyles = converter.styles && converter.styles.markdown;
    if (markdownStyles) {
      for (const style of Object.values(markdownStyles)) {
        if (!style || !style.className) continue;
        if (style.paragraph) {
          paragraphStyles.set(style.className, {
            id: style.className,
            name: style.className,
            paragraph: style.paragraph,
            run: style.run,
          });
        }
        if (style.run && !style.paragraph) {
          characterStyles.set(style.className, {
            id: style.className,
            name: style.className,
            run: style.run,
          });
        }
      }
    }
    if (!numbering && converter.styles && converter.styles.numbering) {
      numbering = converter.styles.numbering;
    }
  }

  return {
    numbering,
    paragraphStyles: Array.from(paragraphStyles.values()),
    characterStyles: Array.from(characterStyles.values()),
  };
}

module.exports = {
  DEFAULT_THEME,
  prepareMarkdown,
  normalizeLatex,
  stripDuplicateTitle,
  stripExecutableHtml,
  renderMarkdownBlocks,
  mergeMarkdownStyles,
};
