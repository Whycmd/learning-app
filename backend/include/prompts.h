// ============================================================================
// prompts.h — AI 提示词集中管理
// ============================================================================
// 所有发给 AI 的 system / user 角色提示词都定义于此。
// 修改提示词只改这一个文件，不混杂在 Service 业务代码中。
//
// 约定：
//   - 使用 static const std::string（编译期常量，免 ODR 问题）
//   - 每个常量上方有注释块，说明：用途、使用者、动态插入点、输出格式
//   - 共享的说明段落（如六板块、规则、JSON schema）抽成公共常量
//   - 需要动态拼接的部分（remark、词表、JSON 数据、数值）留在 Service 层
//
// 使用方：knowledge_service.h / question_service.h / quiz_service.h
// ============================================================================
#pragma once
#include <string>
#include "db.h"

namespace eng {
namespace prompts {

// ==========================================================================
// System 角色（英文，较短，用于设定 AI 角色与输出约束）
// ==========================================================================

/**
 * KP_SOLIDIFY_SYSTEM — 知识点固化 system prompt（单次 / 批量共用）
 * 使用者：KnowledgeService::solidifyOne / solidifyBatch
 * 约束：只输出 JSON，不输出 markdown 或额外解释
 */
static const std::string KP_SOLIDIFY_SYSTEM =
  "You are an English learning assistant. Return ONLY a valid JSON object, no markdown, no explanation."
  "语言规则：所有面向学习者的说明性文本（词根拆解、固定搭配说明、同义词辨析、文化延伸、评语等）用中文输出；目标词、英文例句、英文原文、词性等纯英文内容保持英文。";

/**
 * KP_SOLIDIFY_BATCH_SYSTEM — 批量固化专用 system prompt（强调数组输出）
 * 使用者：KnowledgeService::solidifyBatch
 */
static const std::string KP_SOLIDIFY_BATCH_SYSTEM =
  "You are an English learning assistant. Return ONLY a valid JSON array, no markdown, no explanation."
  "语言规则：所有面向学习者的说明性文本（词根拆解、固定搭配说明、同义词辨析、文化延伸、评语等）用中文输出；目标词、英文例句、英文原文、词性等纯英文内容保持英文。";

/**
 * EXERCISE_GENERATE_SYSTEM — 复习习题生成 system prompt
 * 使用者：QuestionService::generateExercises
 */
static const std::string EXERCISE_GENERATE_SYSTEM =
  "You are an English learning exercise generator. Return ONLY valid JSON."
  "语言规则：题干中的英文内容（挖空句、选项、示例短文）保持英文；所有题目的中文解析、quick_memo、error_analysis 等展示性文本用中文输出。";

/**
 * SUBJECTIVE_GRADE_SYSTEM — 主观题批改 system prompt
 * 使用者：QuestionService::gradeSubjective
 */
static const std::string SUBJECTIVE_GRADE_SYSTEM =
  "You are an English grading assistant. Return ONLY valid JSON."
  "语言规则：评分评语、错误分析(error_analysis)、参考译文(reference_answer 的中文释义) 用中文输出；用户答案、参考答案中的英文原文保持英文。";

/**
 * DAILY_QUIZ_SYSTEM — 每日小测生成 system prompt
 * 使用者：QuizService::generate
 */
static const std::string DAILY_QUIZ_SYSTEM =
  "You are an English daily-quiz generator. Return ONLY valid JSON."
  "语言规则：题干挖空句和选项保持英文；meaning 字段（中文释义）、commentary 等展示性文本用中文输出。";

// ==========================================================================
// 学习者身份（用于 AI 生成时参考难度与侧重方向）
// 可替换：如 "初中生"、"CET-4 考生"、"雅思备考者" 等
// ==========================================================================

/**
 * DEFAULT_LEARNER_IDENTITY — 默认学习者身份（Service 层未指定时使用）
 */
static const std::string DEFAULT_LEARNER_IDENTITY = "高中生";

// ==========================================================================
// 共享段落：六板块说明 + 规则 + JSON schema（单次 / 批量固化共用）
// ==========================================================================

/**
 * KP_SIX_SECTIONS — 六板块说明（单次 / 批量固化 user prompt 共用）
 * 内容：依次要求 AI 输出六个维度的信息
 */
static const std::string KP_SIX_SECTIONS =
  "可以考虑给出：\n"
  "一、核心含义与例句：先给英文核心释义（若过于冗长或难以解释清楚或学习者语言水平较低，则使用中文），再配英文经典例句；每一个含义都要紧接着给出例句。\n"
  "二、词根与词源：词根拆解、词源追溯。\n"
  "三、同源词汇：同根 / 同词族的词。\n"
  "四、典故、俗语与固定搭配：列出固定搭配（争取各给例句），并说明相关典故、俗语与应用场景。\n"
  "五、同义词及辨析：列出同义词，同义替换词，并说明彼此用法差异。\n"
  "六、补充相关知识（重点）：使用频率、语域说明、文化延伸、同类其他词语等。\n"
  "七、根据学习者身份给出其他补充相关知识\n\n";

/**
 * KP_RULES — 固化规则（单次 / 批量固化共用，不含 remark 动态段）
 * 内容：空值处理、篇幅控制、短语特殊解析
 */
static const std::string KP_RULES =
  "规则：\n"
  "- 应当根据实际情况对内容进行增减，如写作高频词汇讲解写作用法\n"
  "- 若某一项不适用，只选择其中价值较高的项输出，应直接略过（字段留空字符串或省略），禁止输出“无 / 没有 / no / none / not applicable”之类说明；\n"
  "- 控制篇幅，讲清重点即可，不要冗长堆砌；\n"
  "- 对较长的短语，对其中包含的核心单词和子短语予以额外解析（填入 phrase_analysis 字段，含同类高频短语和语法考点）。\n\n";

/**
 * KP_REMARK_RULE — 用户备注规则（仅单次固化，动态拼接 remark 值）
 * Service 层使用时需做 std::string replace：将 {REMARK} 替换为实际备注文本
 */
static const std::string KP_REMARK_RULE =
  "- 用户备注: {REMARK}（优先覆盖备注相关内容，无关维度省略）；\n";

/**
 * KP_JSON_SCHEMA — 知识点 JSON schema（对象格式，单次固化用）
 * 输出字段定义
 */
static const std::string KP_JSON_SCHEMA =
  "{\"standard_content\":\"最简英文单词或短语\", \"difficulty\":1-5整数, "
  "\"phonetic\":\"音标如/ˈventʃər/\", "
  "\"definitions\":[{\"pos\":\"词性\",\"meaning\":\"英文核心含义\",\"example\":\"经典例句\"}], "
  "\"root_analysis\":\"词根拆解与词源追溯\", "
  "\"word_family\":[\"同源词汇\"], "
  "\"collocations\":[{\"collocation\":\"固定搭配\",\"example\":\"例句\"}], "
  "\"allusions\":\"典故、俗语与应用场景\", "
  "\"synonyms\":[\"同义词\"], \"synonym_analysis\":\"同义词用法差异\", \"antonyms\":[\"反义词\"], "
  "\"phrase_analysis\":\"(仅短语)内嵌单词/子短语额外解析\", "
  "\"frequency_note\":\"使用频率\", \"register_note\":\"语域说明\", "
  "\"cultural_note\":\"文化延伸\", \"related_words\":\"同类其他词语\"}";

// ==========================================================================
// User 角色模板（中文，较长，用于详细说明任务要求）
// ==========================================================================

/**
 * KP_SOLIDIFY_ONE_USER — 单知识点固化 user prompt 模板
 *
 * 动态插入点：
 *   {USER_INPUT}  — 用户输入的单词 / 短语
 *   {REMARK_LINE} — 可选的 remark 规则行（有备注时才插入，来自 KP_REMARK_RULE 替换后的文本）
 *
 * 使用者：KnowledgeService::solidifyOne
 * 输出格式：单个 JSON 对象（schema 见 KP_JSON_SCHEMA）
 */
static const std::string KP_SOLIDIFY_ONE_USER =
  "你是一位资深英语教研专家。请对下面这个英语单词或短语做一次完整、连贯的精讲，用于学习者的知识点拓展。\n"
  "学习者身份：{LEARNER_IDENTITY}\n（请据此身份对应的学习目标与考察方向调整内容侧重与深度——例如：高中生→高考词汇/语法/长难句/阅读写作；CET-4/6→核心词汇与题型训练；考研→长难句/翻译/写作套路；雅思/托福→听说读写四科应用与学术场景；GRE→高阶学术词汇与批判性阅读。）\n\n"
  "{KP_SIX_SECTIONS}"
  "{KP_RULES}"
  "{REMARK_LINE}"
  "请把内容整理为一个 JSON 对象返回，字段：\n"
  "{KP_JSON_SCHEMA}\n\n"
  "目标词：\"{USER_INPUT}\"\n\n"
  "只输出 JSON 对象，不要任何额外文字。";

/**
 * KP_SOLIDIFY_BATCH_USER — 批量知识点固化 user prompt 模板
 *
 * 动态插入点：
 *   {WORD_LIST} — 编号词表，格式为多行 "1. \"word\"（备注:xxx）" 由 Service 层循环拼接
 *
 * 使用者：KnowledgeService::solidifyBatch
 * 输出格式：JSON 数组，每个元素 schema 同 KP_JSON_SCHEMA
 */
static const std::string KP_SOLIDIFY_BATCH_USER =
  "你是一位资深英语教研专家。下面给你一组英语单词和短语，"
  "请你对其中【每一个】词都做一次完整、连贯的精讲，用于学习者的知识点拓展。\n"
  "学习者身份：{LEARNER_IDENTITY}\n（请据此身份对应的学习目标与考察方向调整内容侧重与深度——例如：高中生→高考词汇/语法/长难句/阅读写作；CET-4/6→核心词汇与题型训练；考研→长难句/翻译/写作套路；雅思/托福→听说读写四科应用与学术场景；GRE→高阶学术词汇与批判性阅读。）\n\n"
  "{KP_SIX_SECTIONS}"
  "{KP_RULES}"
  "请把全部内容整理为一个 JSON 数组返回，每个数组元素对应一个词，顺序与下方词表一致，元素字段：\n"
  "{KP_JSON_SCHEMA}\n\n"
  "词表：\n"
  "{WORD_LIST}\n\n"
  "只输出 JSON 数组，不要任何额外文字。";

// --------------------------------------------------------------------------
// 复习习题生成
// --------------------------------------------------------------------------

/**
 * EXERCISE_GENERATE_USER — 复习习题生成 user prompt
 *
 * 动态插入点：
 *   {MODE_CN}     — "按周（全题型）" 或 "按日（分层抽题）"
 *   {MASTER_REQ}  — "会读" / "会写" / 自定义掌握要求
 *   {PAGE_LIMIT}  — 页数限制数字
 *   {KP_JSON}     — 知识点列表（JSON 字符串，由 Service 层 dump）
 *
 * 使用者：QuestionService::generateExercises
 * 输出格式：{exercises:[...], quick_memo:{items:[...]}}
 * 题型范围：word_match, word_form_fill, context_blank_fill,
 *           cn_to_en_translation, cloze_choice, sentence_imitation
 */
static const std::string EXERCISE_GENERATE_USER =
  "请基于以下知识点集合生成英语习题。\n"
  "学习者身份：{LEARNER_IDENTITY}\n（请据此身份对应的学习目标与考察方向调整题目难度、题型选择与考点偏好——例如：高中生→高考词汇/语法/长难句/阅读写作；CET-4/6→核心词汇与题型训练；考研→长难句/翻译/写作套路；雅思/托福→听说读写四科应用与学术场景；GRE→高阶学术词汇与批判性阅读。）\n"
  "复习模式：{MODE_CN}。"
  "掌握要求：{MASTER_REQ}（会读重辨析理解，会写重变形/填空/翻译/输出）。"
  "页数限制：{PAGE_LIMIT} 页。题目整体难度应低于考察知识点难度，消除阅读障碍。\n"
  "格式可参考以下思路，但可以根据实际情况有所增减：\n"
  "1. 词义匹配题(word_match)建议先提供一篇包含所有目标单词的阅读短文(passage字段)，再出选择题和简答题\n"
  "2. 题型排列顺序可参考：词义匹配→词性转换填空→语境选词填空→中译英翻译→词汇辨析完形选择→例句仿写\n"
  "3. 词汇辨析完形选择(cloze_choice)建议提供一篇含空格的短文(passage)和每空四选项(options)\n"
  "4. quick_memo建议包含所有知识点核心释义速记\n"
  "如有备注/标注，仅考察标注相关内容。\n\n"
  "知识点列表（JSON）：\n{KP_JSON}\n\n"
  "严格按 JSON 输出：{exercises:[{type,question/passage,options?,answer?,knowledge_id,prompt?,reference_answer?,sample?,blanks?}], quick_memo:{items:[{knowledge,brief}]}}\n"
  "题型范围：word_match, word_form_fill, context_blank_fill, cn_to_en_translation, cloze_choice, sentence_imitation";

// --------------------------------------------------------------------------
// 主观题批改
// --------------------------------------------------------------------------

/**
 * SUBJECTIVE_GRADE_USER — 主观题批改 user prompt
 *
 * 动态插入点：
 *   {TYPE}     — 题型名（cn_to_en_translation / sentence_imitation）
 *   {USER_ANS} — 用户答案
 *   {REF_ANS}  — 参考答案（无固定答案时传 "(无固定标准答案)"）
 *
 * 使用者：QuestionService::gradeSubjective
 * 输出格式：{score(0-100 int), correct(bool), error_analysis, reference_answer}
 */
static const std::string SUBJECTIVE_GRADE_USER =
  "请批改以下英语主观题答案。\n"
  "学习者身份：{LEARNER_IDENTITY}\n（批改时请据此身份把握评判尺度——例如：高中生→高考词汇/语法/长难句/阅读写作；CET-4/6→核心词汇与题型训练；考研→长难句/翻译/写作套路；雅思/托福→听说读写四科应用与学术场景；GRE→高阶学术词汇与批判性阅读。）\n"
  "题型：{TYPE}\n"
  "用户答案：{USER_ANS}\n"
  "参考答案：{REF_ANS}\n"
  "严格按 JSON 输出：{score(0-100 int), correct(bool), error_analysis, reference_answer}";

// --------------------------------------------------------------------------
// 每日小测生成（最长、最复杂）
// --------------------------------------------------------------------------

/**
 * DAILY_QUIZ_USER — 每日小测生成 user prompt
 *
 * 动态插入点：
 *   {AVG_DIFF}   — 平均难度（1-5），用于第5题新词难度参考
 *   {KP_JSON}    — 已固化知识点列表（JSON 字符串），来自数据库 solidified 记录
 *
 * 使用者：QuizService::generate
 * 输出格式：{"questions":[{no,category,type,question,options?,answer,knowledge_id?,word?,meaning?,example?}]}
 * 题目构成：严格 3+1+1（3 已固化 + 1 熟词生义 + 1 新知）
 */
static const std::string DAILY_QUIZ_USER =
  "请生成一份5题英语每日小测。\n"
  "学习者身份：{LEARNER_IDENTITY}\n（请据此身份对应的学习目标与考察方向调整题目难度、题型选择与考点偏好——例如：高中生→高考词汇/语法/长难句/阅读写作；CET-4/6→核心词汇与题型训练；考研→长难句/翻译/写作套路；雅思/托福→听说读写四科应用与学术场景；GRE→高阶学术词汇与批判性阅读。）\n"
  "全部为客观题：四选一选择题(type=cloze_choice) "
  "或答案唯一的单词填空题(type=word_form_fill，答案只含一个基础单词)。\n"
  "整体难度略低于正式练习：句子简短(主从句不超过一层)、词汇基础、不含生僻障碍词。\n"
  "【题干硬性要求（解析器依赖，不可省略）】\n"
  "1. 每道题的 question 必须是一个挖空句：在需要填词的位置写 6 个下划线 ______，"
  "例如 \"I can ______ the birds singing outside.\"；\n"
  "2. 题干中绝对不允许出现正确答案单词本身（也不允许出现其大小写变体）；\n"
  "3. 选择题 options 必须是 4 个不带任何编号前缀的纯单词/短语"
  "（禁止写成 \"A) xxx\"，直接写 xxx）；answer 必须为字母 A/B/C/D；"
  "四个选项词性应相同，且只有一个语义正确；\n"
  "4. 填空题 answer 为句中 ______ 处应填单词的正确拼写（一个单词），"
  "答案在该语境下尽量唯一；\n"
  "5. 不要把答案、解析写进 question。\n"
  "题目构成（严格按以下结构与数量，解析器硬依赖）：\n"
  "- 第1-3题 category=\"solidified\"：基于给定知识点出3题，"
  "knowledge_id 必须取自列表真实ID，考点对应该词在列表中的释义；\n"
  "- 第4题 category=\"familiar_meaning\"（熟词生义）："
  "自选一个低级别常见词，考查其不常见义项——应为词典里排在后面、"
  "多数学习者不知道的义项；例如 run 考\"经营\"(✓)，run 考\"跑\"(✗)，"
  "clean 考\"清洁的\"(✗)；\n"
  "- 第5题 category=\"new_kp\"（新知拓展）：自选一个难度贴近学习者水平"
  "(平均难度约{AVG_DIFF}/5)的陌生常用词，考查基本含义；\n"
  "第4、5题必须额外提供 word、meaning(中文释义)、example(英文例句) 字段（解析器依赖）。\n"
  "example 必须用另一个完整真实例句，句中直接写出该词(或其变形)，"
  "绝不能包含 ______，也不能照抄 question；\n"
  "第4、5题若用选择题，answer 指向的选项必须就是 word 本身"
  "(或其在句中的语法变形，如 run→runs)，不要用同义词作答案；\n"
  "第5题请换一个新词，不需要使用 phenomenon、abundant。\n"
  "给定知识点列表(JSON)：\n"
  "{KP_JSON}\n\n"
  "严格只返回JSON。\n"
  "注意：下面的 JSON 仅用于说明字段格式，5 道题内容需要全部重新创作"
  "（包括句子、选项、第4/5题的单词），逐字照抄示例视为错误输出。\n"
  "{\"questions\":[{\"no\":1,\"category\":\"solidified\",\"type\":\"cloze_choice\","
  "\"question\":\"I can ______ the birds singing outside.\","
  "\"options\":[\"sing\",\"hear\",\"perceive\",\"listen\"],\"answer\":\"B\","
  "\"knowledge_id\":12},"
  "{\"no\":4,\"category\":\"familiar_meaning\",\"type\":\"cloze_choice\","
  "\"word\":\"run\",\"meaning\":\"经营；管理\",\"example\":\"He runs a company.\","
  "\"question\":\"The manager ______ the company.\","
  "\"options\":[\"controls\",\"runs\",\"owns\",\"operates\"],\"answer\":\"B\"},"
  "{\"no\":5,\"category\":\"new_kp\",\"type\":\"word_form_fill\","
  "\"word\":\"abundant\",\"meaning\":\"丰富的；充裕的\","
  "\"example\":\"The region has abundant rainfall.\","
  "\"question\":\"Food was ______ during the festival.\",\"answer\":\"abundant\"}]}";

// ==========================================================================
// 辅助函数：模板占位符替换（简单实现，不依赖第三方库）
// 用法：replace(templateStr, "{PLACEHOLDER}", actualValue)
// ==========================================================================

/**
 * 把 src 中的所有 placeholder 替换为 replacement。
 * 如果 placeholder 不存在则原样返回 src。
 */
static std::string replace(const std::string& src,
                           const std::string& placeholder,
                           const std::string& replacement) {
  if (placeholder.empty()) return src;
  std::string result;
  result.reserve(src.size() + replacement.size() * 2);
  size_t pos = 0, prev = 0;
  while ((pos = src.find(placeholder, prev)) != std::string::npos) {
    result.append(src, prev, pos - prev);
    result.append(replacement);
    prev = pos + placeholder.size();
  }
  result.append(src, prev, src.size() - prev);
  return result;
}

/**
 * 链式替换便捷函数：对 template 依次替换 pairs 中的每个 (placeholder, replacement)。
 * pairs 是一个 vector<pair<string, string>>。
 */
static std::string buildTemplate(const std::string& tmpl,
                                 const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::string result = tmpl;
  for (auto& p : pairs) {
    result = replace(result, p.first, p.second);
  }
  return result;
}


/**
 * 根据用户 ID 获取学习者身份字符串。
 * 游客（userId <= 1）或查询失败 → 返回 DEFAULT_LEARNER_IDENTITY。
 * 供 Service 层在调用 buildTemplate 时替换 {LEARNER_IDENTITY} 占位符。
 */
static std::string getLearnerIdentity(Db& db, int userId)
{
  if (userId <= 1) return DEFAULT_LEARNER_IDENTITY;
  try
  {
    auto rows = db.query(
      "SELECT learner_identity FROM users WHERE id=" + std::to_string(userId) + " LIMIT 1"
    );
    if (rows.empty()) return DEFAULT_LEARNER_IDENTITY;
    std::string id = rows[0]["learner_identity"];
    return id.empty() ? DEFAULT_LEARNER_IDENTITY : id;
  }
  catch (...) { return DEFAULT_LEARNER_IDENTITY; }
}

} // namespace prompts
} // namespace eng
