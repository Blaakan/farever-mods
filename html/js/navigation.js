(function () {
  'use strict';

  function knownCharacters() {
    const data = window.FAREVER_REPORT_DATA || {};
    const names = [...(data.inventories || []), ...(data.jobs || [])]
      .map((entry) => entry && entry.character).filter(Boolean);
    return [...new Set(names)].sort((a, b) => a.localeCompare(b, 'fr'));
  }

  function menus() {
    const characters = knownCharacters();
    return [
      { label: 'Accueil', href: 'index.html' },
      { label: 'Hors-jeu', children: [
        ['Tous les personnages', 'farever-report.html'],
        ...characters.map((name) => [name, `farever-report.html?hero=${encodeURIComponent(name)}`])
      ]}
    ];
  }

  const escapeHtml = (value) => String(value)
    .replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');

  const style = `
    .fmk-nav{position:fixed;left:0;right:0;top:0;z-index:1000;background:#0b1217ee;border-bottom:1px solid #2b3b45;backdrop-filter:blur(12px);font:14px system-ui,sans-serif}
    .fmk-has-fixed-nav{padding-top:62px}
    .fmk-nav__inner{max-width:1180px;min-height:62px;margin:auto;padding:0 24px;display:flex;align-items:center;gap:24px}
    .fmk-nav__brand{display:flex;align-items:center;gap:10px;color:#edf2ef;text-decoration:none;font:700 17px Georgia,serif;white-space:nowrap}
    .fmk-nav__logo{width:35px;height:35px;display:grid;place-items:center;border:1px solid #e7bd68;border-radius:10px;color:#e7bd68;background:linear-gradient(145deg,#29352b,#11191e);font:700 15px Georgia,serif;box-shadow:0 0 16px #e7bd6820}
    .fmk-nav__menu{display:flex;align-items:center;gap:5px;margin-left:auto}.fmk-nav__item{position:relative}
    .fmk-nav a,.fmk-nav button{font:inherit}.fmk-nav__link,.fmk-nav__toggle{display:flex;align-items:center;gap:6px;border:0;background:transparent;color:#cbd6d1;text-decoration:none;padding:10px 11px;border-radius:8px;cursor:pointer}
    .fmk-nav__link:hover,.fmk-nav__toggle:hover,.fmk-nav__toggle[aria-expanded=true]{background:#1b272e;color:#fff}.fmk-nav__chevron{font-size:10px;color:#e7bd68}
    .fmk-nav__submenu{position:absolute;right:0;top:calc(100% + 7px);min-width:220px;margin:0;padding:7px;list-style:none;background:#111a20;border:1px solid #31434e;border-radius:11px;box-shadow:0 16px 38px #0008;display:none}
    .fmk-nav__item.open>.fmk-nav__submenu{display:block}.fmk-nav__submenu a{display:block;color:#cbd6d1;text-decoration:none;padding:9px 10px;border-radius:7px}.fmk-nav__submenu a:hover,.fmk-nav__submenu a:focus{background:#24333c;color:#fff;outline:none}
    .fmk-nav__burger{display:none;margin-left:auto;width:42px;height:40px;border:1px solid #31434e;border-radius:9px;background:#121c22;cursor:pointer;padding:9px}.fmk-nav__burger span{display:block;height:2px;background:#e7bd68;margin:4px 0;transition:.2s}
    @media(max-width:760px){.fmk-nav__inner{padding:0 16px}.fmk-nav__burger{display:block}.fmk-nav__menu{display:none;position:absolute;left:12px;right:12px;top:calc(100% + 7px);margin:0;padding:8px;align-items:stretch;flex-direction:column;background:#0f181e;border:1px solid #31434e;border-radius:12px;box-shadow:0 18px 42px #0009}.fmk-nav.menu-open .fmk-nav__menu{display:flex}.fmk-nav__link,.fmk-nav__toggle{width:100%;justify-content:space-between;padding:12px}.fmk-nav__submenu{position:static;min-width:0;margin:3px 0 5px;box-shadow:none;background:#0b1318}.fmk-nav__item.open>.fmk-nav__submenu{display:block}}
  `;

  function closeAll(nav, except) {
    nav.querySelectorAll('.fmk-nav__item.open').forEach((item) => {
      if (item === except) return;
      item.classList.remove('open');
      item.querySelector('.fmk-nav__toggle')?.setAttribute('aria-expanded', 'false');
    });
  }

  function createFareverNavigation(target, options = {}) {
    const mount = typeof target === 'string' ? document.querySelector(target) : target;
    if (!mount) return null;
    if (!document.getElementById('fmk-nav-style')) {
      const sheet = document.createElement('style');
      sheet.id = 'fmk-nav-style'; sheet.textContent = style; document.head.appendChild(sheet);
    }
    const nav = document.createElement('nav');
    nav.className = 'fmk-nav';
    nav.setAttribute('aria-label', 'Navigation principale');
    const items = menus().map((entry, index) => {
      if (!entry.children) return `<div class="fmk-nav__item"><a class="fmk-nav__link" href="${entry.href}">${escapeHtml(entry.label)}</a></div>`;
      const id = `fmk-submenu-${index}`;
      return `<div class="fmk-nav__item"><button class="fmk-nav__toggle" type="button" aria-expanded="false" aria-controls="${id}">${entry.label}<span class="fmk-nav__chevron">▼</span></button><ul class="fmk-nav__submenu" id="${id}">${entry.children.map(([label, href]) => `<li><a href="${href}">${escapeHtml(label)}</a></li>`).join('')}</ul></div>`;
    }).join('');
    nav.innerHTML = `<div class="fmk-nav__inner"><a class="fmk-nav__brand" href="index.html"><span class="fmk-nav__logo" aria-hidden="true">FM</span><span>${options.title || 'Farever Modkit'}</span></a><button class="fmk-nav__burger" type="button" aria-label="Ouvrir le menu" aria-expanded="false"><span></span><span></span><span></span></button><div class="fmk-nav__menu">${items}</div></div>`;
    mount.replaceWith(nav);
    document.body.classList.add('fmk-has-fixed-nav');
    const burger = nav.querySelector('.fmk-nav__burger');
    burger.addEventListener('click', () => {
      const open = nav.classList.toggle('menu-open');
      burger.setAttribute('aria-expanded', String(open));
      burger.setAttribute('aria-label', open ? 'Fermer le menu' : 'Ouvrir le menu');
      if (!open) closeAll(nav);
    });
    nav.querySelectorAll('.fmk-nav__toggle').forEach((button) => button.addEventListener('click', () => {
      const item = button.closest('.fmk-nav__item'), open = !item.classList.contains('open');
      closeAll(nav, item); item.classList.toggle('open', open); button.setAttribute('aria-expanded', String(open));
    }));
    nav.querySelectorAll('a').forEach((link) => link.addEventListener('click', () => { nav.classList.remove('menu-open'); burger.setAttribute('aria-expanded', 'false'); closeAll(nav); }));
    document.addEventListener('click', (event) => { if (!nav.contains(event.target)) { nav.classList.remove('menu-open'); burger.setAttribute('aria-expanded', 'false'); closeAll(nav); } });
    document.addEventListener('keydown', (event) => { if (event.key === 'Escape') { nav.classList.remove('menu-open'); burger.setAttribute('aria-expanded', 'false'); closeAll(nav); burger.focus(); } });
    return nav;
  }

  window.createFareverNavigation = createFareverNavigation;
  document.addEventListener('DOMContentLoaded', () => document.querySelectorAll('[data-farever-navigation]').forEach((node) => createFareverNavigation(node)));
})();