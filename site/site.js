(() => {
  "use strict";

  const root = document.documentElement;
  const toggle = document.querySelector(".theme-toggle");
  const preference = "twisted-cable-manager-site-theme";
  const applyTheme = (theme) => {
    root.dataset.theme = theme;
    const dark = theme === "dark";
    toggle.setAttribute("aria-pressed", String(!dark));
    toggle.setAttribute("aria-label", `Switch to ${dark ? "light" : "dark"} theme`);
    toggle.querySelector(".theme-label").textContent = dark ? "Light" : "Dark";
  };

  let theme = "dark";
  try {
    const saved = localStorage.getItem(preference);
    if (saved === "light" || saved === "dark") theme = saved;
  } catch (_) {}
  applyTheme(theme);
  toggle.addEventListener("click", () => {
    theme = root.dataset.theme === "dark" ? "light" : "dark";
    applyTheme(theme);
    try { localStorage.setItem(preference, theme); } catch (_) {}
  });

  const cards = [...document.querySelectorAll("[data-screen]")];
  if (cards.length) {
    const filters = [...document.querySelectorAll("[data-filter]")];
    let selected = "all";
    const update = () => {
      let visible = 0;
      cards.forEach(card => {
        card.hidden = selected !== "all" && card.dataset.group !== selected;
        if (!card.hidden) visible++;
      });
      filters.forEach(button => button.setAttribute("aria-pressed", String(button.dataset.filter === selected)));
      document.querySelector("#screen-count").textContent = `${visible} ${visible === 1 ? "screen" : "screens"}`;
    };
    filters.forEach(button => button.addEventListener("click", () => { selected = button.dataset.filter; update(); }));
  }

  const input = document.querySelector("#faq-search");
  if (!input) return;
  const entries = [...document.querySelectorAll("[data-faq]")];
  const groups = [...document.querySelectorAll(".faq-group")];
  const status = document.querySelector("#search-status");
  const empty = document.querySelector("#no-results");
  const normalize = (value) => value.normalize("NFKC").toLocaleLowerCase();
  const index = entries.map((entry) => normalize(entry.textContent));
  const search = () => {
    const words = normalize(input.value.trim()).split(/\s+/).filter(Boolean);
    let visible = 0;
    entries.forEach((entry, position) => {
      entry.hidden = !words.every((word) => index[position].includes(word));
      if (!entry.hidden) visible += 1;
    });
    groups.forEach((group) => {
      group.hidden = !group.querySelector("[data-faq]:not([hidden])");
    });
    status.textContent = words.length ? `${visible} of ${entries.length} questions match.` : `${entries.length} questions. Search titles and answers.`;
    empty.hidden = visible !== 0;
  };
  const clear = () => { input.value = ""; search(); input.focus(); };
  input.addEventListener("input", search);
  input.addEventListener("keydown", (event) => {
    if (event.key === "Escape") { event.preventDefault(); clear(); }
  });
  document.querySelector(".search-clear").addEventListener("click", clear);
  document.querySelector(".doc-nav").addEventListener("click", (event) => {
    if (event.target.closest("a") && input.value) { input.value = ""; search(); }
  });
  search();
})();
