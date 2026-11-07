(function () {
  var top = document.querySelector("[data-top]");
  var menu = document.querySelector("[data-menu]");
  var nav = document.querySelector("[data-nav]");
  var play = document.querySelector("[data-play]");
  var stop = document.querySelector("[data-stop]");
  var playLabel = document.querySelector("[data-play-label]");
  var lanes = document.querySelector("[data-lanes]");
  var clock = document.querySelector("[data-clock]");
  var cpu = document.querySelector("[data-cpu]");
  var meters = Array.prototype.slice.call(document.querySelectorAll("[data-meter]"));

  var reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var playing = false;
  var pos = 0.18;
  var origin = 0.18;
  var last = 0;
  var raf = 0;
  var loopSeconds = 8;

  function pad(n, width) {
    var s = String(n);
    while (s.length < width) s = "0" + s;
    return s;
  }

  function formatClock(seconds) {
    var clamped = Math.max(0, seconds);
    var m = Math.floor(clamped / 60);
    var s = Math.floor(clamped % 60);
    var ms = Math.floor((clamped % 1) * 1000);
    return pad(m, 2) + ":" + pad(s, 2) + "." + pad(ms, 3);
  }

  function render() {
    if (lanes) lanes.style.setProperty("--pos", String(pos));
    if (clock) clock.textContent = formatClock(pos * loopSeconds);
  }

  function paint(now) {
    render();
    if (!playing) return;
    if (!last) last = now;
    var dt = Math.min(0.05, (now - last) / 1000);
    last = now;
    pos += dt / loopSeconds;
    if (pos >= 1) pos -= 1;
    meters.forEach(function (bar, i) {
      var wave = 0.35 + 0.5 * Math.abs(Math.sin(now / 280 + i * 0.9));
      if (i === meters.length - 1) wave = 0.55 + 0.3 * Math.abs(Math.sin(now / 420));
      bar.style.height = Math.round(wave * 100) + "%";
    });
    raf = window.requestAnimationFrame(paint);
  }

  function setPlaying(next) {
    playing = next;
    if (play) play.setAttribute("aria-pressed", playing ? "true" : "false");
    if (playLabel) playLabel.textContent = playing ? "Pause" : "Play";
    if (playing) {
      last = 0;
      if (reduce) {
        pos = 0.62;
        render();
        if (cpu) cpu.textContent = "—";
        return;
      }
      raf = window.requestAnimationFrame(paint);
      return;
    }
    if (raf) window.cancelAnimationFrame(raf);
    raf = 0;
    if (cpu) cpu.textContent = "—";
    render();
  }

  function reset() {
    pos = origin;
    setPlaying(false);
  }

  if (menu && top && nav) {
    menu.addEventListener("click", function () {
      var open = top.classList.toggle("nav-open");
      menu.setAttribute("aria-expanded", open ? "true" : "false");
    });
    nav.addEventListener("click", function (event) {
      if (event.target.closest("a")) {
        top.classList.remove("nav-open");
        menu.setAttribute("aria-expanded", "false");
      }
    });
    document.addEventListener("keydown", function (event) {
      if (event.key === "Escape" && top.classList.contains("nav-open")) {
        top.classList.remove("nav-open");
        menu.setAttribute("aria-expanded", "false");
        menu.focus();
      }
    });
  }

  if (play) {
    play.addEventListener("click", function () {
      setPlaying(!playing);
    });
  }
  if (stop) {
    stop.addEventListener("click", reset);
  }

  render();
})();
