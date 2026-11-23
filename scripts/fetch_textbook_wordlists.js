#!/usr/bin/env node
/**
 * 下载人教版（2019）高中英语 7 册词表，生成 backend/data/textbook_wordlists/pep2019.tsv
 *
 * 数据源：https://word.threeher.cn/books/gaozhong/（2026-11-08 核实，人教最新版 7 册）
 * 只抓取单词与「册 / 单元」定位，不抓释义、例句。
 * 生成结果人工可直接编辑；改完重启后端会自动重灌内置词表。
 *
 * 用法：node scripts/fetch_textbook_wordlists.js [--out 路径]
 */

const fs = require('fs');
const path = require('path');

const BASE = 'https://word.threeher.cn';
const BOOKS = [
  { volume: '必修第一册', id: 'e9ac90beb5189ad6' },
  { volume: '必修第二册', id: 'fc57ad1e7843ffba' },
  { volume: '必修第三册', id: '3d3d03ad9ad9d9f2' },
  { volume: '选择性必修第一册', id: 'c0c8f3f73e542329' },
  { volume: '选择性必修第二册', id: 'a57e6f3f20138843' },
  { volume: '选择性必修第三册', id: 'f9a4edbb2fdd0a62' },
  { volume: '选择性必修第四册', id: '0f637dab02acc432' }
];
const TEXTBOOK = '人教版（2019）';
const DEFAULT_OUT = path.join(__dirname, '..', 'backend', 'data', 'textbook_wordlists', 'pep2019.tsv');

function decodeEntities(text)
{
  return text
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&nbsp;/g, ' ')
    .trim();
}

async function fetchText(url)
{
  const res = await fetch(url, { headers: { 'User-Agent': 'Mozilla/5.0 (compatible; eng-app-wordlist-fetch/1.0)' } });
  if (!res.ok)
  {
    throw new Error(`HTTP ${res.status} ${url}`);
  }
  return res.text();
}

function parseUnitLinks(html, book)
{
  const re = new RegExp(`href="(/books/${book.id}/[^"/]+/)"[^>]*>([^<]+)</a>`, 'g');
  const prefix = `人教版高中英语${book.volume}`;
  const seen = new Set();
  const units = [];
  let match;

  while ((match = re.exec(html)) !== null)
  {
    const href = match[1];
    const label = decodeEntities(match[2]);

    if (!label.startsWith(prefix) || !label.endsWith('单词表') || seen.has(href))
    {
      continue;
    }

    const unit = label.slice(prefix.length, -'单词表'.length).trim();
    if (!unit)
    {
      continue;
    }

    seen.add(href);
    units.push({ unit, url: BASE + href });
  }

  return units;
}

function parseStatedCount(html)
{
  const match = html.match(/本单元共\s*(?:<[^>]+>\s*)?(\d+)/);
  return match ? parseInt(match[1], 10) : null;
}

function parseWords(html)
{
  const re = /<span class="cp-word">([^<]+)<\/span>/g;
  const words = [];
  let match;

  while ((match = re.exec(html)) !== null)
  {
    const word = decodeEntities(match[1]).replace(/\s+/g, ' ').trim();
    if (word)
    {
      words.push(word);
    }
  }

  return words;
}

function dedupeWords(words)
{
  const seen = new Set();
  const result = [];

  for (const word of words)
  {
    const key = word.toLocaleLowerCase();
    if (seen.has(key))
    {
      continue;
    }
    seen.add(key);
    result.push(word);
  }

  return result;
}

async function fetchBook(book)
{
  const bookUrl = `${BASE}/books/${book.id}/`;
  const bookHtml = await fetchText(bookUrl);
  const units = parseUnitLinks(bookHtml, book);

  if (units.length === 0)
  {
    throw new Error(`${book.volume} 未解析到任何单元：${bookUrl}`);
  }

  const rows = [];

  for (const unit of units)
  {
    const html = await fetchText(unit.url);
    const rawWords = parseWords(html);
    const stated = parseStatedCount(html);

    if (rawWords.length === 0)
    {
      throw new Error(`${book.volume} ${unit.unit} 未解析到单词：${unit.url}`);
    }

    if (stated !== null && stated !== rawWords.length)
    {
      throw new Error(`${book.volume} ${unit.unit} 词数不一致：页面 ${stated}，解析 ${rawWords.length}`);
    }

    const words = dedupeWords(rawWords);
    if (words.length !== rawWords.length)
    {
      console.log(`\n  ${book.volume} ${unit.unit} 去除重复 ${rawWords.length - words.length} 条`);
    }

    words.forEach((word, index) => rows.push({
      volume: book.volume,
      unit: unit.unit,
      order_no: index + 1,
      word
    }));

    await new Promise(resolve => setTimeout(resolve, 250));
  }

  return { book, units, rows };
}

async function main()
{
  const outIndex = process.argv.indexOf('--out');
  const outPath = outIndex > -1 && process.argv[outIndex + 1]
    ? path.resolve(process.argv[outIndex + 1])
    : DEFAULT_OUT;

  const results = [];

  for (const book of BOOKS)
  {
    process.stdout.write(`下载 ${book.volume} ... `);
    const result = await fetchBook(book);
    results.push(result);
    process.stdout.write(`${result.units.length} 单元 / ${result.rows.length} 词\n`);
    await new Promise(resolve => setTimeout(resolve, 250));
  }

  const lines = ['textbook\tvolume\tunit\torder_no\tword'];
  for (const result of results)
  {
    for (const row of result.rows)
    {
      lines.push([TEXTBOOK, row.volume, row.unit, row.order_no, row.word].join('\t'));
    }
  }

  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  fs.writeFileSync(outPath, `${lines.join('\n')}\n`, 'utf8');

  const total = results.reduce((sum, result) => sum + result.rows.length, 0);
  const unitCount = results.reduce((sum, result) => sum + result.units.length, 0);

  console.log(`\n已写入 ${outPath}`);
  console.log(`共 ${BOOKS.length} 册 / ${unitCount} 单元 / ${total} 词\n`);

  for (const result of results)
  {
    console.log(`${result.book.volume}\t${result.units.length} 单元\t${result.rows.length} 词`);
    for (const unit of result.units)
    {
      const count = result.rows.filter(row => row.unit === unit.unit).length;
      console.log(`  ${unit.unit}\t${count}`);
    }
  }
}

main().catch(error =>
{
  console.error(`\n抓取失败：${error.message}`);
  process.exit(1);
});
