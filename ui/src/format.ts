// Small shared formatting, in one place because three panels showed the same
// duration three different ways before it was.

export function formatTime(milliseconds: number): string {
  if (!Number.isFinite(milliseconds) || milliseconds <= 0) {
    // An em dash pair rather than 0:00, because "we do not know how long this is"
    // and "this is zero seconds long" are different facts and the second one is
    // never true of a track.
    return '--:--';
  }
  const total = Math.floor(milliseconds / 1000);
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  return `${minutes}:${seconds.toString().padStart(2, '0')}`;
}

/** For an album or a whole library, where minutes and seconds are not the point. */
export function formatDuration(milliseconds: number): string {
  const minutes = Math.round(milliseconds / 60000);
  if (minutes < 60) {
    return `${minutes} min`;
  }
  const hours = Math.floor(minutes / 60);
  return `${hours} h ${minutes % 60} min`;
}

/**
 * A count, with the separators the reader's own locale uses.
 *
 * 50000 is a number you have to stop and count the digits of; 50,000 is one you
 * read. The library routinely holds five figures of tracks, so this is not a
 * detail.
 */
export function formatCount(value: number): string {
  return value.toLocaleString();
}

/**
 * `1 track`, `50,000 tracks`, and never `50000 track(s)`.
 *
 * The parenthesised plural was in eleven strings in this interface. It is the
 * kind of thing that reads as a placeholder somebody forgot to finish, because
 * that is exactly what it is.
 */
export function plural(count: number, singular: string, many = `${singular}s`): string {
  return `${formatCount(count)} ${count === 1 ? singular : many}`;
}

export function element<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className !== undefined) {
    node.className = className;
  }
  if (text !== undefined) {
    // textContent, never innerHTML. Every string here came out of a tag in
    // somebody's music file, which is to say out of the internet.
    node.textContent = text;
  }
  return node;
}

/* ---- icons --------------------------------------------------------------- */

const kSvgNamespace = 'http://www.w3.org/2000/svg';

interface Glyph {
  readonly box: number;
  readonly d: readonly string[];
  /** Solid shapes (the transport) rather than outlines (everything else). */
  readonly filled?: boolean;
  /** Round caps look right on an open stroke and wrong on a closed one. */
  readonly caps?: boolean;
}

// Drawn as paths rather than pulled from an icon font, for the reason the whole
// page is drawn the way it is: it is served from sonora:// out of bytes compiled
// into the executable, and there is no network behind that scheme to fetch a font
// from. Sixteen-unit boxes, so the shapes land on whole pixels at 16px.
const kGlyphs = {
  tracks: { box: 16, d: ['M2.5 4h11M2.5 8h11M2.5 12h7'], caps: true },
  albums: {
    box: 16,
    d: [
      'M3.25 2.25h3a1 1 0 0 1 1 1v3a1 1 0 0 1-1 1h-3a1 1 0 0 1-1-1v-3a1 1 0 0 1 1-1z',
      'M9.75 2.25h3a1 1 0 0 1 1 1v3a1 1 0 0 1-1 1h-3a1 1 0 0 1-1-1v-3a1 1 0 0 1 1-1z',
      'M3.25 8.75h3a1 1 0 0 1 1 1v3a1 1 0 0 1-1 1h-3a1 1 0 0 1-1-1v-3a1 1 0 0 1 1-1z',
      'M9.75 8.75h3a1 1 0 0 1 1 1v3a1 1 0 0 1-1 1h-3a1 1 0 0 1-1-1v-3a1 1 0 0 1 1-1z',
    ],
  },
  artists: {
    box: 16,
    d: ['M8 2.75a2.75 2.75 0 1 1 0 5.5 2.75 2.75 0 0 1 0-5.5z', 'M3 13.25c0-2.2 2.24-3.75 5-3.75s5 1.55 5 3.75'],
  },
  folder: { box: 16, d: ['M1.75 4.25a1 1 0 0 1 1-1h3.2l1.3 1.5h5a1 1 0 0 1 1 1v6.5a1 1 0 0 1-1 1h-9.5a1 1 0 0 1-1-1z'] },
  search: { box: 16, d: ['M7 2.75a4.25 4.25 0 1 1 0 8.5 4.25 4.25 0 0 1 0-8.5z', 'M10.2 10.2 13.5 13.5'], caps: true },
  rescan: { box: 16, d: ['M13.5 8a5.5 5.5 0 1 1-1.7-3.97', 'M13.6 2.5v3.1h-3.1'], caps: true },
  previous: { box: 18, d: ['M5 4h2v10H5z', 'M14 4v10l-7-5z'], filled: true },
  play: { box: 18, d: ['M5.6 3.4v11.2l9-5.6z'], filled: true },
  pause: { box: 18, d: ['M5.6 3.5h2.2a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H5.6a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1z',
                        'M10.2 3.5h2.2a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1h-2.2a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1z'],
           filled: true },
  next: { box: 18, d: ['M11 4h2v10h-2z', 'M4 4v10l7-5z'], filled: true },
  stop: { box: 18, d: ['M6.2 4.2h5.6a2 2 0 0 1 2 2v5.6a2 2 0 0 1-2 2H6.2a2 2 0 0 1-2-2V6.2a2 2 0 0 1 2-2z'], filled: true },
  volume: { box: 18, d: ['M4 6.8h2.4L9.8 4v10L6.4 11.2H4z', 'M12.2 6.6a3.2 3.2 0 0 1 0 4.8'], caps: true },
  muted: { box: 18, d: ['M4 6.8h2.4L9.8 4v10L6.4 11.2H4z', 'M12.4 6.9l3 3.2', 'M15.4 6.9l-3 3.2'], caps: true },
  send: { box: 16, d: ['M8 13V3.5', 'M4.2 7.2L8 3.4l3.8 3.8'], caps: true },
  plus: { box: 16, d: ['M8 4v8M4 8h8'], caps: true },
  close: { box: 16, d: ['M4.5 4.5l7 7M11.5 4.5l-7 7'], caps: true },
  done: { box: 16, d: ['M3 8.6l3 3 7-7'], caps: true },
  warning: { box: 16, d: ['M8 1.9 14.6 13.4H1.4z', 'M8 5.6v3.3'], caps: true },
  theme: { box: 16, d: ['M8 2.2a5.8 5.8 0 1 0 0 11.6 4.6 4.6 0 0 1 0-11.6z'] },
} as const satisfies Record<string, Glyph>;

export type IconName = keyof typeof kGlyphs;

/** One inline SVG, built node by node so that no string here is ever parsed as markup. */
export function icon(name: IconName, size = 16): SVGSVGElement {
  const glyph: Glyph = kGlyphs[name];
  const root = document.createElementNS(kSvgNamespace, 'svg');
  root.setAttribute('width', String(size));
  root.setAttribute('height', String(size));
  root.setAttribute('viewBox', `0 0 ${glyph.box} ${glyph.box}`);
  root.setAttribute('fill', glyph.filled === true ? 'currentColor' : 'none');
  root.setAttribute('aria-hidden', 'true');
  if (glyph.filled !== true) {
    root.setAttribute('stroke', 'currentColor');
    root.setAttribute('stroke-width', '1.5');
    if (glyph.caps === true) {
      root.setAttribute('stroke-linecap', 'round');
      root.setAttribute('stroke-linejoin', 'round');
    }
  }
  for (const d of glyph.d) {
    const shape = document.createElementNS(kSvgNamespace, 'path');
    shape.setAttribute('d', d);
    root.append(shape);
  }
  return root;
}

/* ---- covers -------------------------------------------------------------- */

/**
 * Three bars, the marque from the application icon.
 *
 * Used for two different absences and one presence: a track with no artwork and
 * nothing to derive a monogram from, an empty library, and the row that is
 * playing. The same shape in all three places is the point -- it is the one mark
 * this interface has of its own.
 */
export function barMark(className = 'bars', heights: readonly number[] = [0.45, 1, 0.7]): HTMLElement {
  const mark = element('span', className);
  for (const height of heights) {
    const bar = element('i');
    bar.style.setProperty('--bar', String(height));
    mark.append(bar);
  }
  return mark;
}

/**
 * A stable number per string, so the same album is the same colour every time it
 * is drawn -- in the list, in the grid, in the queue and in the transport.
 *
 * FNV-1a over the UTF-16 units. Not a hash anybody depends on: it picks a hue.
 */
function hue(text: string): number {
  let value = 0x811c9dc5;
  for (let index = 0; index < text.length; index += 1) {
    value ^= text.charCodeAt(index);
    value = Math.imul(value, 0x01000193);
  }
  return Math.abs(value) % 360;
}

/**
 * Two characters: the initials of the first two words, or the first two letters
 * when there is only one word.
 *
 * One letter was the first attempt and it looked wrong on screen in a way it did
 * not on paper -- "Tideworks" became "T" while "Long Shore" became "LS", so half
 * the covers in a list were a different shape from the other half. Two is the
 * rule, whatever the name is made of.
 */
function monogram(label: string): string {
  const words = label.split(/[\s\-_/(]+/u).filter((word) => /\p{L}|\p{N}/u.test(word));
  if (words.length === 0) {
    return '';
  }
  const letters =
    words.length === 1
      ? [...(words[0] ?? '')].slice(0, 2).join('')
      : [...words.slice(0, 2)].map((word) => [...word][0] ?? '').join('');
  return letters.toUpperCase();
}

/**
 * A cover, or something derived from the tags when the file carries no artwork.
 *
 * Most files do not carry artwork, so this is not the edge case it sounds like --
 * it is what a list of fifty thousand tracks mostly looks like. A single letter
 * on one grey was what it looked like before, which made every row identical and
 * the whole list read as broken. Two initials on a hue derived from the album
 * name means the same album always looks the same and a scroll has landmarks in
 * it, without a byte of artwork or a network request.
 */
export function coverElement(artUrl: string, label: string, className = 'cover'): HTMLElement {
  if (artUrl === '') {
    const placeholder = element('div', `${className} cover-empty`);
    const initials = monogram(label);
    if (initials === '') {
      // Nothing to derive from: no title, no album, no artist. The bars.
      placeholder.classList.add('cover-blank');
      placeholder.append(barMark('bars bars-cover'));
      return placeholder;
    }
    placeholder.style.setProperty('--cover-h', String(hue(label)));
    placeholder.textContent = initials;
    return placeholder;
  }
  const image = element('img', className);
  image.src = artUrl;
  image.alt = '';
  // The URL is a content hash, so the bytes behind it can never change and the
  // shell says so in the response. Lazy because an album grid is a few hundred
  // images and only a dozen are on screen.
  image.loading = 'lazy';
  image.decoding = 'async';
  return image;
}

/**
 * The matched part of a string, wrapped in `<mark>`.
 *
 * Built out of text nodes and one element rather than by splicing tags into a
 * string, for the same reason everything else here is: the haystack came out of
 * a music file. A search that highlights nothing makes the reader do the
 * matching the computer just did.
 */
export function highlight(text: string, query: string, className = 'text'): HTMLElement {
  const host = element('span', className);
  const needle = query.trim().toLowerCase();
  if (needle === '') {
    host.textContent = text;
    return host;
  }

  let from = 0;
  const haystack = text.toLowerCase();
  for (;;) {
    const at = haystack.indexOf(needle, from);
    if (at < 0) {
      break;
    }
    if (at > from) {
      host.append(text.slice(from, at));
    }
    host.append(element('mark', undefined, text.slice(at, at + needle.length)));
    from = at + needle.length;
  }
  host.append(text.slice(from));
  return host;
}
