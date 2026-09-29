import { CapabilitySet } from './bridge/capabilities';
import { type LibraryAlbum, type LibraryStatus, type LibraryTrack } from './bridge/generated';
import { BridgeError, sonora } from './bridge/invoke';
import {
  barMark,
  coverElement,
  element,
  formatCount,
  formatDuration,
  formatTime,
  highlight,
  icon,
  plural,
} from './format';
import { themeToggle } from './theme';
import { arraySource, type RowSource, VirtualList } from './virtual_list';

// The library: a sidebar of places to be, a list of tracks, and a search box.
//
// Everything it draws came from the index, which means every list is a query
// over SQLite rather than a walk of the filesystem -- and every track is an id.
// The page never sees a file path and could not name one if it wanted to, which
// is what week 7's second half was for.
//
// Two things about how it looks are load-bearing rather than taste. The rows are a
// constant 44px because the virtual list turns a scroll offset into a row index by
// dividing by that number. And the columns are one grid declared once in the
// stylesheet, shared by the header and every row: before that there was no header
// at all and each row sized its own columns, which is how a list of tracks ends up
// with the album name starting at a different place on every line.

/** Tracks are fetched a page at a time as the window moves over them. */
const kPageSize = 500;

interface LibraryActions {
  /** Replaces the queue with these tracks and starts playing. */
  play(trackIds: readonly number[]): Promise<void>;
  /** Adds to the end of the queue, leaving playback alone. */
  enqueue(trackIds: readonly number[]): Promise<void>;
  /** The track the player is on, or 0. The list marks it; it does not own it. */
  currentTrackId(): number;
  /** So the marks can be redrawn when the player moves to another track. */
  onCurrentChanged(listener: () => void): void;
}

interface LibraryRoots {
  readonly sidebar: HTMLElement;
  readonly content: HTMLElement;
  readonly search: HTMLElement;
}

type View =
  | { readonly kind: 'tracks' }
  | { readonly kind: 'albums' }
  | { readonly kind: 'artists' }
  | { readonly kind: 'album'; readonly album: LibraryAlbum }
  | { readonly kind: 'artist'; readonly artist: string }
  | { readonly kind: 'search'; readonly query: string };

type AlbumSort = 'artist' | 'title' | 'year';

function describe(error: unknown): string {
  if (error instanceof BridgeError) {
    return `${error.message} (code ${error.code})`;
  }
  return error instanceof Error ? error.message : String(error);
}

/**
 * Every track in the library, fetched in pages as the list is scrolled.
 *
 * The alternative -- one call for the whole library -- would serialise fifty
 * thousand records to JSON, send them across the process boundary and parse them
 * again, to draw forty rows. This holds `total` from the first call, so the
 * scrollbar is the right size immediately, and fills in the rest as it is
 * needed.
 */
class PagedTracks implements RowSource<LibraryTrack> {
  private readonly pages = new Map<number, readonly LibraryTrack[]>();
  private readonly pending = new Set<number>();
  private count = 0;

  constructor(private readonly onArrived: () => void) {}

  get length(): number {
    return this.count;
  }

  async first(): Promise<void> {
    const page = await sonora.library.listTracks({ limit: kPageSize, offset: 0 });
    this.count = page.total;
    this.pages.set(0, page.tracks);
  }

  item(index: number): LibraryTrack | undefined {
    const page = Math.floor(index / kPageSize);
    return this.pages.get(page)?.[index - page * kPageSize];
  }

  ensure(first: number, last: number): void {
    for (let page = Math.floor(first / kPageSize); page <= Math.floor(last / kPageSize); page += 1) {
      if (this.pages.has(page) || this.pending.has(page)) {
        continue;
      }
      this.pending.add(page);
      void sonora.library
        .listTracks({ limit: kPageSize, offset: page * kPageSize })
        .then((result) => {
          this.pages.set(page, result.tracks);
          this.count = result.total;
          // The rows that were drawn as pending are now drawable.
          this.onArrived();
        })
        .catch(() => {
          // Left out of `pages`, so scrolling back over it tries again. A failed
          // page is not a reason to break the list.
        })
        .finally(() => {
          this.pending.delete(page);
        });
    }
  }
}

export class LibraryView {
  private readonly navigation = element('nav', 'nav');
  private readonly rootPath = element('span');
  private readonly rootRow = element('button', 'library-root');
  private readonly scan = element('div', 'scan');
  private readonly scanCount = element('span', 'scan-count');
  private readonly scanFill = element('div', 'scan-fill');
  private readonly note = element('p', 'library-note');

  private readonly input = element('input', 'search-input');
  private readonly clearSearch = element('button', 'search-clear');

  private readonly head = element('div', 'content-head');
  private readonly heading = element('h2', 'heading');
  private readonly subheading = element('p', 'subheading');
  private readonly actions = element('div', 'content-actions');
  private readonly message = element('p', 'message');
  private readonly tableHead = element('div', 'table-head');
  private readonly list = element('div', 'library-list');
  private readonly grid = element('div', 'library-grid');
  private readonly empty = element('div', 'empty');

  private tracks: VirtualList<LibraryTrack> | undefined;
  private view: View = { kind: 'tracks' };
  private searchTimer: ReturnType<typeof setTimeout> | undefined;
  private lastStatus: LibraryStatus | undefined;
  private available = false;
  /**
   * Whether this shell can open a folder chooser.
   *
   * library@2 is the version that has the method. An older shell running a newer
   * page is the case the capability negotiation exists for, and the honest
   * behaviour is not a button that fails: it is no button, and the sentence about
   * --library that was the whole story before.
   */
  private canChoose = false;
  private albums: readonly LibraryAlbum[] = [];
  private albumSort: AlbumSort = 'artist';
  /** The ids currently listed, in order, so clicking one can queue the rest after it. */
  private listed: readonly number[] = [];
  /** What to highlight in a row. Empty in every view but a search. */
  private needle = '';

  constructor(
    private readonly roots: LibraryRoots,
    private readonly actionsApi: LibraryActions,
  ) {}

  async mount(capabilities: CapabilitySet): Promise<void> {
    if (!capabilities.has('library')) {
      this.roots.sidebar.replaceChildren(
        this.brand(),
        element(
          'p',
          'library-note',
          capabilities.isDisabled('library')
            ? 'The library is switched off for this run.'
            : 'This shell has no library.',
        ),
      );
      this.roots.content.replaceChildren();
      this.roots.search.replaceChildren();
      return;
    }

    this.available = true;
    this.canChoose = capabilities.has('library', 2);
    this.build();

    try {
      const status = await sonora.library.getStatus();
      this.onStatus(status.library);
    } catch (error) {
      this.note.textContent = describe(error);
      this.note.dataset['kind'] = 'bad';
    }
    await this.show({ kind: 'tracks' });
  }

  /** From the library.status event, and from the first getStatus. */
  onStatus(status: LibraryStatus): void {
    if (!this.available) {
      return;
    }
    const previous = this.lastStatus;
    this.lastStatus = status;

    this.rootPath.textContent = status.root === '' ? 'No folder' : status.root;
    this.rootPath.title = status.root;

    // What used to be one long sentence in the sidebar is now three things in
    // three places: the counts are on the nav items they count, the progress is a
    // bar, and what is left here is the part that is genuinely prose.
    this.scan.hidden = !status.scanning;
    if (status.scanning) {
      // There is no total to divide by: filesSeen is how many the walk has
      // reached so far, not how many there are. A bar claiming a percentage would
      // be inventing the denominator, so it sweeps instead and the numbers say
      // what is actually known.
      this.scanFill.dataset['indeterminate'] = 'true';
      this.scanCount.textContent =
        `${formatCount(status.filesRead)} of ${formatCount(status.filesSeen)} read`;
    }

    this.note.textContent = this.statusNote(status);
    this.note.dataset['kind'] = status.lastError === '' ? 'muted' : 'bad';
    this.note.hidden = this.note.textContent === '';

    this.updateNavigation();

    // Redraw when a scan finishes, or when it has changed the counts while
    // running: the list on screen is a query result from before those rows
    // existed.
    const finished = previous?.scanning === true && !status.scanning;
    const grew = previous !== undefined && previous.trackCount !== status.trackCount;
    if (finished || grew) {
      void this.show(this.view);
    }
  }

  private statusNote(status: LibraryStatus): string {
    if (status.lastError !== '') {
      return status.lastError;
    }
    if (status.scanning) {
      // The bar says what is happening; this says the one thing the bar cannot,
      // and only when there is something to say.
      return status.failed > 0
        ? `${plural(status.failed, 'file')} could not be read`
        : '';
    }
    if (status.root === '' || status.filesSeen === 0) {
      return '';
    }
    // "read 0 of 3,000" is the incremental scan working, and it is the one number
    // in this window worth keeping on screen after a rescan: without it, pressing
    // Rescan on an unchanged folder is indistinguishable from pressing nothing.
    const changes = status.added + status.updated + status.removed;
    return (
      `Last scan: ${formatCount(status.filesRead)} of ${formatCount(status.filesSeen)} files read` +
      (changes === 0
        ? ', nothing changed'
        : `, +${formatCount(status.added)} ~${formatCount(status.updated)} ` +
          `-${formatCount(status.removed)}`)
    );
  }

  private brand(): HTMLElement {
    const brand = element('div', 'brand');
    brand.append(
      // The application icon, drawn in CSS: four rounded bars, three lit and one
      // not. It is in the installer and on the taskbar already; this is the same
      // mark, and it is why the accent in this window is that blue.
      barMark('brand-mark', [0.38, 0.72, 1, 0.5]),
      element('span', 'brand-name', 'Sonora'),
    );
    return brand;
  }

  private build(): void {
    /* ---- search bar ---- */

    this.input.type = 'search';
    this.input.id = 'library-search';
    this.input.placeholder = 'Search titles, artists, albums';
    const label = element('label', 'sr-only', 'Search the library');
    label.htmlFor = this.input.id;

    this.input.addEventListener('input', () => {
      this.clearSearch.hidden = this.input.value === '';
      // Debounced: a search box that calls on every keystroke sends five
      // requests for a five-letter word and races their answers onto the screen.
      if (this.searchTimer !== undefined) {
        clearTimeout(this.searchTimer);
      }
      this.searchTimer = setTimeout(() => {
        const query = this.input.value.trim();
        void this.show(query === '' ? { kind: 'tracks' } : { kind: 'search', query });
      }, 120);
    });
    this.input.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && this.input.value !== '') {
        event.stopPropagation();
        this.resetSearch();
      }
    });

    this.clearSearch.type = 'button';
    this.clearSearch.setAttribute('aria-label', 'Clear the search');
    this.clearSearch.title = 'Clear the search';
    this.clearSearch.append(icon('close', 10));
    this.clearSearch.hidden = true;
    this.clearSearch.addEventListener('click', () => this.resetSearch());

    // Chromium's own clear button is hidden in the stylesheet, because it is a
    // different shape and colour from every other control on the page.
    const searchIcon = icon('search', 15);
    searchIcon.classList.add('search-icon');

    const field = element('div', 'search-field');
    field.append(
      searchIcon,
      label,
      this.input,
      this.clearSearch,
      element('span', 'search-hint', 'Ctrl K'),
    );

    const rescan = element('button', 'rescan');
    rescan.type = 'button';
    rescan.title = 'Look for files that changed since the last scan';
    rescan.append(icon('rescan', 14), 'Rescan');
    rescan.addEventListener('click', () => this.rescan());

    this.roots.search.replaceChildren(field, rescan);

    // The shortcut every search box on this machine has. Without it the field is
    // reachable only with the pointer, which is the wrong ergonomics for the one
    // control that is used more than any other.
    document.addEventListener('keydown', (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
        event.preventDefault();
        this.input.focus();
        this.input.select();
      }
    });

    /* ---- sidebar ---- */

    this.scan.append(
      (() => {
        const labels = element('div', 'scan-labels');
        labels.append(element('span', undefined, 'Indexing'), this.scanCount);
        return labels;
      })(),
      (() => {
        const track = element('div', 'scan-track');
        track.append(this.scanFill);
        return track;
      })(),
    );
    this.scan.hidden = true;

    // The folder is a button when the shell can open a chooser and a plain line
    // when it cannot, rather than a button that is there and disabled: a control
    // that exists and refuses is a worse answer than one that was never offered.
    this.rootRow.type = 'button';
    this.rootRow.append(icon('folder', 14), this.rootPath);
    if (this.canChoose) {
      this.rootRow.title = 'Choose another folder to index';
      this.rootRow.addEventListener('click', () => this.chooseFolder());
    } else {
      this.rootRow.disabled = true;
      this.rootRow.dataset['static'] = 'true';
    }

    const foot = element('div', 'library-foot');
    foot.append(this.rootRow, this.scan, this.note, themeToggle());

    this.roots.sidebar.replaceChildren(
      this.brand(),
      this.navigation,
      element('div', 'sidebar-spacer'),
      foot,
    );
    this.updateNavigation();

    /* ---- content ---- */

    // Classed rather than left to nth-child: the narrow layout drops the album
    // column, and a rule that hides "the fourth span" is a rule that hides the
    // wrong thing the day a column is added.
    this.tableHead.append(
      element('span', 'col-number', '#'),
      element('span', 'col-title', 'Title'),
      element('span', 'col-artist', 'Artist'),
      element('span', 'col-album', 'Album'),
      element('span', 'col-time', 'Time'),
      element('span', 'col-actions'),
    );

    this.tracks = new VirtualList<LibraryTrack>(this.list, {
      // The stylesheet's --row, read once rather than written twice. A number
      // typed in both places is a number that will disagree with itself.
      rowHeight:
        Number.parseFloat(
          getComputedStyle(document.documentElement).getPropertyValue('--row'),
        ) || 44,
      renderRow: (track, index) => this.trackRow(track, index),
      renderPending: () => element('div', 'track-row-pending', '\u2026'),
    });

    const titles = element('div', 'content-titles');
    titles.append(this.heading, this.subheading);
    this.head.append(titles, this.actions);

    this.roots.content.replaceChildren(
      this.head,
      this.message,
      this.tableHead,
      this.list,
      this.grid,
      this.empty,
    );

    // The mark on the playing row comes from the player, and the player moves on
    // its own. invalidate() redraws the rows that are on screen, which is the
    // cheapest correct answer: forty elements, no refetch.
    this.actionsApi.onCurrentChanged(() => this.tracks?.invalidate());
  }

  private resetSearch(): void {
    this.input.value = '';
    this.clearSearch.hidden = true;
    void this.show({ kind: 'tracks' });
  }

  /**
   * Opens the system's folder chooser.
   *
   * There is nothing to await: the call returns when the dialog is up, not when
   * it is answered -- see the schema's note on why a bridge call must not wait
   * for a person. What they chose comes back as the `root` of the next
   * library.status, through onStatus, like every other fact about the library.
   */
  private chooseFolder(): void {
    void sonora.library
      .chooseFolder()
      .then((result) => {
        if (!result.opened) {
          this.note.hidden = false;
          this.note.dataset['kind'] = 'muted';
          this.note.textContent = 'A folder chooser is already open.';
        }
      })
      .catch((error: unknown) => {
        this.note.hidden = false;
        this.note.dataset['kind'] = 'bad';
        this.note.textContent = describe(error);
      });
  }

  private rescan(): void {
    // Answered immediately, before any event arrives. A scan of an unchanged
    // folder is over in milliseconds and leaves the status text identical to what
    // it already said, so without this the button looks broken -- which is what it
    // looked like.
    this.note.hidden = false;
    this.note.dataset['kind'] = 'muted';
    this.note.textContent = 'Scanning…';
    void sonora.library
      .scan({})
      .then((result) => {
        if (!result.started) {
          this.note.textContent = 'A scan is already running.';
        }
      })
      .catch((error: unknown) => {
        this.note.textContent = describe(error);
        this.note.dataset['kind'] = 'bad';
      });
  }

  private updateNavigation(): void {
    const status = this.lastStatus;
    const entries = [
      { label: 'All tracks', count: status?.trackCount, view: { kind: 'tracks' } as View },
      { label: 'Albums', count: status?.albumCount, view: { kind: 'albums' } as View },
      { label: 'Artists', count: status?.artistCount, view: { kind: 'artists' } as View },
    ] as const;

    this.navigation.replaceChildren(
      ...entries.map(({ label, count, view }) => {
        const button = element('button', 'nav-item');
        button.type = 'button';
        button.append(
          icon(view.kind === 'tracks' ? 'tracks' : view.kind === 'albums' ? 'albums' : 'artists'),
          element('span', 'nav-label', label),
          element('span', 'nav-count', count === undefined ? '' : formatCount(count)),
        );
        // An album or artist page is a place inside its section, so the section
        // stays lit while you are in one.
        const active =
          this.view.kind === view.kind ||
          (view.kind === 'albums' && this.view.kind === 'album') ||
          (view.kind === 'artists' && this.view.kind === 'artist');
        button.dataset['active'] = String(active);
        button.addEventListener('click', () => void this.show(view));
        return button;
      }),
    );
  }

  private async show(view: View): Promise<void> {
    this.view = view;
    this.updateNavigation();
    this.message.textContent = '';
    this.needle = view.kind === 'search' ? view.query : '';

    const status = this.lastStatus;
    if (status !== undefined && status.root === '') {
      this.showEmpty(this.noFolder());
      return;
    }

    try {
      switch (view.kind) {
        case 'tracks': {
          const source = new PagedTracks(() => this.tracks?.invalidate());
          await source.first();
          if (source.length === 0) {
            this.showEmpty(this.nothingIndexed(status));
            break;
          }
          this.showList('All tracks', `${plural(source.length, 'track')} · album order`, source);
          // The ids for "play from here" are only known page by page. Clicking a
          // row in this view queues that track and everything after it that has
          // arrived, which is what a listener means by it.
          this.listed = [];
          break;
        }
        case 'albums': {
          const result = await sonora.library.listAlbums();
          this.albums = result.albums;
          if (this.albums.length === 0) {
            this.showEmpty(this.nothingIndexed(status));
            break;
          }
          this.showAlbums();
          break;
        }
        case 'artists': {
          const result = await sonora.library.listArtists();
          if (result.artists.length === 0) {
            this.showEmpty(this.nothingIndexed(status));
            break;
          }
          this.showArtists(result.artists);
          break;
        }
        case 'album': {
          const result = await sonora.library.albumTracks({
            album: view.album.album,
            albumArtist: view.album.albumArtist,
          });
          this.showList(
            view.album.album === '' ? 'Unknown album' : view.album.album,
            [
              view.album.albumArtist,
              view.album.year > 0 ? String(view.album.year) : '',
              plural(result.tracks.length, 'track'),
              formatDuration(view.album.durationMs),
            ]
              .filter((part) => part !== '')
              .join(' · '),
            arraySource(result.tracks),
            result.tracks,
            view.album,
          );
          break;
        }
        case 'artist': {
          const result = await sonora.library.artistTracks({ artist: view.artist });
          this.showList(
            view.artist,
            plural(result.tracks.length, 'track'),
            arraySource(result.tracks),
            result.tracks,
          );
          break;
        }
        case 'search': {
          const result = await sonora.library.search({ query: view.query });
          if (result.tracks.length === 0) {
            this.showEmpty(this.noResults(view.query, status));
            break;
          }
          this.showList(
            'Search results',
            `${plural(result.tracks.length, 'match', 'matches')} for “${view.query}” · best first`,
            arraySource(result.tracks),
            result.tracks,
          );
          break;
        }
      }
    } catch (error) {
      this.message.textContent = describe(error);
    }
  }

  /** One of the three panes is shown at a time, and this is the only place that decides. */
  private showPane(which: 'list' | 'grid' | 'empty'): void {
    this.tableHead.hidden = which !== 'list';
    this.list.hidden = which !== 'list';
    this.grid.hidden = which !== 'grid';
    this.empty.hidden = which !== 'empty';
    // An empty state says what it is in its own words. A heading above it saying
    // "Library" over a blank subheading is a leftover from the pane that is not
    // being shown.
    this.head.hidden = which === 'empty';
  }

  private showList(
    heading: string,
    subheading: string,
    source: RowSource<LibraryTrack>,
    tracks?: readonly LibraryTrack[],
    album?: LibraryAlbum,
  ): void {
    this.heading.replaceChildren(
      ...(album !== undefined
        ? [coverElement(album.artUrl, album.album || album.albumArtist, 'cover cover-md')]
        : []),
      element('span', undefined, heading),
    );
    this.subheading.textContent = subheading;
    this.listed = tracks?.map((track) => track.id) ?? [];

    this.actions.replaceChildren();
    if (tracks !== undefined && tracks.length > 0) {
      const play = element('button', 'button button-primary', 'Play all');
      play.type = 'button';
      play.addEventListener('click', () => void this.actionsApi.play(this.listed));
      const add = element('button', 'button', 'Queue all');
      add.type = 'button';
      add.addEventListener('click', () => void this.actionsApi.enqueue(this.listed));
      this.actions.append(play, add);
    }

    this.grid.replaceChildren();
    this.showPane('list');
    this.tracks?.setSource(source);
  }

  private trackRow(track: LibraryTrack, index: number): HTMLElement {
    const row = element('div', 'track-row');
    row.dataset['id'] = String(track.id);
    if (track.id !== 0 && track.id === this.actionsApi.currentTrackId()) {
      row.dataset['current'] = 'true';
    }

    const lead = element('span', 'track-lead');
    lead.append(
      element(
        'span',
        'track-number',
        String(track.trackNumber > 0 ? track.trackNumber : index + 1),
      ),
      (() => {
        const play = element('span', 'track-play');
        play.append(icon('play', 13));
        return play;
      })(),
      barMark('bars track-mark'),
    );

    const title = element('span', 'track-cell');
    title.append(
      // The album decides the colour, so every track of an album looks like the
      // same album -- in this list, in the queue, and in the bar at the bottom.
      coverElement(track.artUrl, track.album || track.albumArtist || track.title, 'cover cover-sm'),
      highlight(track.title, this.needle, 'track-title'),
    );

    const add = element('button', 'track-add');
    add.type = 'button';
    add.setAttribute('aria-label', `Add ${track.title} to the queue`);
    add.title = 'Add to the queue';
    add.append(icon('plus', 14));
    add.addEventListener('click', (event) => {
      // Or the click would also count as a click on the row.
      event.stopPropagation();
      void this.actionsApi.enqueue([track.id]);
    });

    row.append(
      lead,
      title,
      highlight(track.artist, this.needle, 'track-artist'),
      highlight(track.album, this.needle, 'track-album'),
      element('span', 'track-time', formatTime(track.durationMs)),
      add,
    );

    row.addEventListener('dblclick', () => {
      // This track, then the rest of what is on screen after it -- which is what
      // clicking a track in a list means to a listener. In the paged view the
      // rest is not known yet, so it is this track alone.
      const from = this.listed.indexOf(track.id);
      void this.actionsApi.play(from >= 0 ? this.listed.slice(from) : [track.id]);
    });
    return row;
  }

  private showAlbums(): void {
    this.heading.replaceChildren(element('span', undefined, 'Albums'));
    const withoutArt = this.albums.filter((album) => album.artUrl === '').length;
    this.subheading.textContent =
      plural(this.albums.length, 'album') +
      (withoutArt > 0 ? ` · ${formatCount(withoutArt)} without cover art` : '');
    this.listed = [];

    // Sorted here rather than in SQL. Five hundred rows are already in memory and
    // the index's own order is the one the track list needs; a second ORDER BY
    // over the bridge to reorder a list the page is holding would be a round trip
    // to do what a comparator does.
    const sorts: ReadonlyArray<{ key: AlbumSort; label: string }> = [
      { key: 'artist', label: 'Artist' },
      { key: 'title', label: 'Title' },
      { key: 'year', label: 'Year' },
    ];
    const segments = element('div', 'segments');
    segments.setAttribute('aria-label', 'Sort the albums');
    for (const { key, label } of sorts) {
      const button = element('button', 'segment', label);
      button.type = 'button';
      button.dataset['active'] = String(this.albumSort === key);
      button.addEventListener('click', () => {
        this.albumSort = key;
        this.showAlbums();
      });
      segments.append(button);
    }
    this.actions.replaceChildren(segments);

    // Untagged last, in all three orders. An empty string sorts before every real
    // name and a 0 before every real year, so the naive comparator opens the page
    // on every file nobody ever tagged -- which is the least interesting thing in
    // the library and the first thing you would see.
    const untagged = (text: string, other: string): number =>
      text === '' ? (other === '' ? 0 : 1) : other === '' ? -1 : 0;

    const ordered = [...this.albums].sort((left, right) => {
      if (this.albumSort === 'year') {
        const missing =
          (left.year === 0 ? 1 : 0) - (right.year === 0 ? 1 : 0);
        if (missing !== 0) {
          return missing;
        }
        // Newest first among the ones that have a year at all.
        return right.year - left.year || left.album.localeCompare(right.album);
      }
      if (this.albumSort === 'title') {
        return (
          untagged(left.album, right.album) || left.album.localeCompare(right.album)
        );
      }
      return (
        untagged(left.albumArtist, right.albumArtist) ||
        left.albumArtist.localeCompare(right.albumArtist) ||
        left.year - right.year
      );
    });

    // Not virtualized: an album grid is hundreds of cards, not tens of thousands,
    // and each one is a lazily loaded image the browser skips until it is near the
    // viewport. The track list is where the row count gets serious.
    this.grid.replaceChildren(
      ...ordered.map((album) => {
        const card = element('button', 'album-card');
        card.type = 'button';

        const art = element('span', 'album-art');
        const shade = element('span', 'album-shade');
        const badge = element('span');
        badge.append(icon('play', 16));
        shade.append(badge);
        art.append(coverElement(album.artUrl, album.album || album.albumArtist), shade);

        card.append(
          art,
          element('span', 'album-name', album.album === '' ? 'Unknown album' : album.album),
          element(
            'span',
            'album-meta',
            [album.albumArtist || 'Unknown artist', album.year > 0 ? String(album.year) : '']
              .filter((part) => part !== '')
              .join(' · '),
          ),
        );
        card.addEventListener('click', () => void this.show({ kind: 'album', album }));
        return card;
      }),
    );
    this.showPane('grid');
  }

  private showArtists(artists: readonly string[]): void {
    this.heading.replaceChildren(element('span', undefined, 'Artists'));
    this.subheading.textContent = plural(artists.length, 'artist');
    this.actions.replaceChildren();
    this.listed = [];

    this.grid.replaceChildren(
      ...artists.map((artist) => {
        const card = element('button', 'artist-card');
        card.type = 'button';
        card.append(
          coverElement('', artist, 'cover cover-sm'),
          element('span', undefined, artist),
        );
        card.addEventListener('click', () => void this.show({ kind: 'artist', artist }));
        return card;
      }),
    );
    this.showPane('grid');
  }

  /* ---- the three ways a pane can be empty ------------------------------- */
  //
  // Each of these says what happened, why, and what to do about it. The version
  // before this said "Nothing indexed yet." for all three, which is a sentence
  // that makes the reader work out which of the three they are looking at.

  // The nodes go straight into `.empty` rather than into a wrapper, because
  // `.empty` is what centres them. A wrapper is one block-level box in a centred
  // column, and everything inside it goes back to being left-aligned -- which is
  // how the bar mark ended up off to one side of its own title.
  private showEmpty(nodes: readonly Node[]): void {
    this.listed = [];
    this.empty.replaceChildren(...nodes);
    this.showPane('empty');
  }

  private emptyBlock(title: string, body: Node | string): HTMLElement {
    const block = element('div');
    block.append(element('p', 'empty-title', title));
    const paragraph = element('p', 'empty-body');
    paragraph.append(body);
    block.append(paragraph);
    return block;
  }

  private noFolder(): readonly Node[] {
    const body = document.createDocumentFragment();
    body.append(
      'Sonora indexes one folder. It reads the tags, keeps its index beside itself, and ' +
        'never writes to your files.',
    );

    const nodes: Node[] = [
      barMark('bars empty-mark', [0.36, 0.68, 1, 0.5]),
      this.emptyBlock('Point Sonora at your music', body),
    ];

    if (this.canChoose) {
      const actions = element('div', 'empty-actions');
      const choose = element('button', 'button button-primary', 'Choose a folder');
      choose.type = 'button';
      choose.addEventListener('click', () => this.chooseFolder());
      actions.append(choose);
      nodes.push(actions);

      // The flag still works and is still the right tool for a shortcut, a test
      // or a second library, so it is said once, quietly, under the button rather
      // than instead of it.
      const hint = element('p', 'empty-hint');
      hint.append('Or start it with ', element('code', undefined, '--library "C:\\Music"'), '.');
      nodes.push(hint);
    } else {
      // An older shell, which has no chooser. The flag is not a hint here, it is
      // the only way, so it is the sentence.
      const only = element('p', 'empty-body');
      only.append(
        'This build has no folder chooser. Start it with ',
        element('code', undefined, '--library "C:\\Music"'),
        '.',
      );
      nodes.push(only);
    }
    return nodes;
  }

  private nothingIndexed(status: LibraryStatus | undefined): readonly Node[] {
    const scanning = status?.scanning === true;
    const nodes: Node[] = [
      barMark('bars empty-mark', [0.3, 0.55, 0.4]),
      this.emptyBlock(
        scanning ? 'Indexing' : 'Nothing in the index yet',
        scanning
          ? 'Rows appear here as they are found; the list is usable while the scan runs.'
          : 'The folder is set but the index is empty. Either it holds no audio files Sonora can read, or it has not been scanned yet.',
      ),
    ];
    if (!scanning) {
      const actions = element('div', 'empty-actions');
      const rescan = element('button', 'button button-primary', 'Scan now');
      rescan.type = 'button';
      rescan.addEventListener('click', () => this.rescan());
      actions.append(rescan);
      nodes.push(actions);
    }
    return nodes;
  }

  private noResults(query: string, status: LibraryStatus | undefined): readonly Node[] {
    const title = element('p', 'empty-title');
    title.append('No track, artist or album named ', element('span', 'query', query));

    const body = element('p', 'empty-body');
    body.append(
      status === undefined
        ? 'Nothing matched.'
        : `${plural(status.trackCount, 'track')} searched. If you have just added files, a rescan will pick them up — or ask for it in words in the Ask tab.`,
    );

    const actions = element('div', 'empty-actions');
    const rescan = element('button', 'button', 'Rescan');
    rescan.type = 'button';
    rescan.addEventListener('click', () => this.rescan());
    const clear = element('button', 'button button-quiet', 'Clear the search');
    clear.type = 'button';
    clear.addEventListener('click', () => this.resetSearch());
    actions.append(rescan, clear);

    const block = element('div');
    block.append(title, body);
    return [icon('search', 30), block, actions];
  }
}
