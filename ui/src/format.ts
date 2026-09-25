// Small shared formatting, in one place because three panels showed the same
// duration three different ways before it was.

export function formatTime(milliseconds: number): string {
  if (!Number.isFinite(milliseconds) || milliseconds < 0) {
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

/** A cover, or a square with a letter in it when the file had no artwork. */
export function coverElement(artUrl: string, label: string, className = 'cover'): HTMLElement {
  if (artUrl === '') {
    const placeholder = element('div', `${className} cover-empty`);
    placeholder.textContent = label.trim().charAt(0).toUpperCase() || '♪';
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
