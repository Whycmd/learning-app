// tts.js — 朗读工具模块（v1.2.0-T006）
// 本期使用浏览器 SpeechSynthesis，后续可切换为后端 TTS 服务
const TTS = (() => {
  let currentUtterance = null;

  /**
   * 朗读指定文本
   * @param {string} text 要朗读的英文文本
   * @param {object} opts 可选配置：{ lang: 'en-US', rate: 0.9 }
   */
  function speak(text, opts = {}) {
    if (!('speechSynthesis' in window)) {
      console.warn('[TTS] speechSynthesis 不可用');
      return;
    }
    window.speechSynthesis.cancel(); // 中断上一条
    const u = new SpeechSynthesisUtterance(text || '');
    u.lang = opts.lang || 'en-US';
    u.rate = opts.rate !== undefined ? opts.rate : 0.9;
    u.pitch = 1;
    currentUtterance = u;
    window.speechSynthesis.speak(u);
  }

  /**
   * 停止当前朗读
   */
  function stop() {
    window.speechSynthesis.cancel();
    currentUtterance = null;
  }

  /**
   * 预留：后续接入后端朗读服务时的切换入口
   * 当前降级为本地 speechSynthesis
   * @param {string} word
   * @param {object} opts
   */
  async function speakRemote(word, opts = {}) {
    // TODO: v1.3.0 接入后端 TTS 服务
    // const r = await fetch('/api/tts/play', {
    //   method: 'POST',
    //   headers: { 'Content-Type': 'application/json' },
    //   body: JSON.stringify({ word, ...opts })
    // });
    // return r.json();
    speak(word, opts);
  }

  return { speak, stop, speakRemote };
})();
