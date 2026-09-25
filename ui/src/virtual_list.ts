// A list that draws only the rows you can see.
//
// The reason this exists: a library of fifty thousand tracks is fifty thousand
// rows, and a row is a handful of DOM nodes. Building them all costs seconds and
// hundreds of megabytes, and Chromium then spends every frame on layout for rows
// nobody is looking at. So the viewport is a window: the scrollable area is as
// tall as all the rows put together, and the forty that are actually on screen
// are positioned inside it.
//
// The one thing that makes this work without measuring anything is a fixed row
// height. Rows of different heights would mean the scroll position no longer
// maps to an index arithmetically, and the answer to that is a measured,
// cached, invalidated layout -- which is a real piece of engineering and not one
// this week needs, because a track list is a table and tables have rows of one
// height.

/** Where the rows come from. `item` may return undefined for a row whose data has not arrived yet. */
export interface RowSource<T> {
  readonly length: number;
  item(index: number): T | undefined;
  /** Called with the visible range, so a paged source can fetch what is missing. */
  ensure?(first: number, last: number): void;
}

export interface VirtualListOptions<T> {
  readonly rowHeight: number;
  /** Rows drawn above and below the viewport, so a fast scroll does not show gaps. */
  readonly overscan?: number;
  readonly renderRow: (item: T, index: number) => HTMLElement;
  /** Drawn for a row whose data is still on its way. */
  readonly renderPending: (index: number) => HTMLElement;
}

const emptySource: RowSource<never> = { length: 0, item: () => undefined };

export class VirtualList<T> {
  private readonly spacer = document.createElement('div');
  private readonly rows = document.createElement('div');
  private source: RowSource<T> = emptySource;
  private frame = 0;
  private firstDrawn = -1;
  private countDrawn = 0;

  constructor(
    private readonly viewport: HTMLElement,
    private readonly options: VirtualListOptions<T>,
  ) {
    this.viewport.classList.add('virtual-viewport');
    this.spacer.className = 'virtual-spacer';
    this.rows.className = 'virtual-rows';
    this.spacer.append(this.rows);
    this.viewport.replaceChildren(this.spacer);

    this.viewport.addEventListener('scroll', () => this.schedule(), { passive: true });
    // The window's height decides how many rows are needed, and it changes when
    // the window does. Without this, resizing shows blank space until the next
    // scroll.
    new ResizeObserver(() => this.schedule()).observe(this.viewport);
  }

  setSource(source: RowSource<T>, { keepScroll = false } = {}): void {
    this.source = source;
    if (!keepScroll) {
      this.viewport.scrollTop = 0;
    }
    // Forces a redraw even if the visible range happens to be the same one.
    this.firstDrawn = -1;
    this.draw();
  }

  /** Data arrived for rows that were drawn as pending. */
  invalidate(): void {
    this.firstDrawn = -1;
    this.schedule();
  }

  private schedule(): void {
    if (this.frame !== 0) {
      return;
    }
    // One draw per frame at most: a scroll fires far more often than the screen
    // refreshes, and drawing per event is how a list feels heavy while doing the
    // same work several times over.
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      this.draw();
    });
  }

  private draw(): void {
    const { rowHeight } = this.options;
    const overscan = this.options.overscan ?? 6;
    const total = this.source.length;

    this.spacer.style.height = `${total * rowHeight}px`;
    if (total === 0) {
      this.rows.replaceChildren();
      this.firstDrawn = -1;
      this.countDrawn = 0;
      return;
    }

    const visible = Math.ceil(this.viewport.clientHeight / rowHeight) + 1;
    const first = Math.max(0, Math.floor(this.viewport.scrollTop / rowHeight) - overscan);
    const count = Math.min(total - first, visible + overscan * 2);

    if (first === this.firstDrawn && count === this.countDrawn) {
      return;  // scrolled within the rows already drawn
    }
    this.firstDrawn = first;
    this.countDrawn = count;

    this.source.ensure?.(first, first + count - 1);

    const drawn: HTMLElement[] = [];
    for (let index = first; index < first + count; index += 1) {
      const item = this.source.item(index);
      const row = item === undefined
        ? this.options.renderPending(index)
        : this.options.renderRow(item, index);
      row.style.height = `${rowHeight}px`;
      drawn.push(row);
    }

    // The whole window is replaced rather than diffed. Forty elements is
    // nothing, and a reuse pool would be the second thing to get wrong after the
    // scroll arithmetic. If profiling ever says otherwise, this is where it goes.
    this.rows.style.transform = `translateY(${first * rowHeight}px)`;
    this.rows.replaceChildren(...drawn);
  }
}

/** A source over a list already in memory. */
export function arraySource<T>(items: readonly T[]): RowSource<T> {
  return { length: items.length, item: (index) => items[index] };
}
