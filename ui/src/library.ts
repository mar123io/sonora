import { CapabilitySet } from './bridge/capabilities';
import { type LibraryAlbum, type LibraryStatus, type LibraryTrack } from './bridge/generated';
import { BridgeError, sonora } from './bridge/invoke';
import { coverElement, element, formatDuration, formatTime } from './format';
import { arraySource, type RowSource, VirtualList } from './virtual_list';

// The library: a sidebar of places to be, a list of tracks, and a search box.
//
// Everything it draws came from the index, which means every list is a query
// over SQLite rather than a walk of the filesystem -- and every track is an id.
// The page never sees a file path and could not name one if it wanted to, which
// is what week 7's second half was for.

/** Tracks are fetched a page at a time as the window moves over them. */
const kPageSize = 500;

interface LibraryActions {
  /** Replaces the queue with these tracks and starts playing. */
  play(trackIds: readonly number[]): Promise<void>;
  /** Adds to the end of the queue, leaving playback alone. */
  enqueue(trackIds: readonly number[]): Promise<void>;
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
  private readonly status = element('p', 'library-status');
  private readonly heading = element('h2', 'library-heading');
  private readonly subheading = element('p', 'library-subheading');
  private readonly list = element('div', 'library-list');
  private readonly grid = element('div', 'library-grid');
  private readonly message = element('p', 'library-message');
  private readonly input = element('input', 'library-search');
  private readonly navigation = element('nav', 'library-nav');

  private tracks: VirtualList<LibraryTrack> | undefined;
  private view: View = { kind: 'tracks' };
  private searchTimer: ReturnType<typeof setTimeout> | undefined;
  private lastStatus: LibraryStatus | undefined;
  private available = false;
  /** The ids currently listed, in order, so clicking one can queue the rest after it. */
  private listed: readonly number[] = [];

  constructor(
    private readonly roots: LibraryRoots,
    private readonly actions: LibraryActions,
  ) {}

  async mount(capabilities: CapabilitySet): Promise<void> {
    if (!capabilities.has('library')) {
      this.roots.sidebar.replaceChildren(
        element(
          'p',
          'unavailable',
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
    this.build();

    try {
      const status = await sonora.library.getStatus();
      this.onStatus(status.library);
    } catch (error) {
      this.status.textContent = describe(error);
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

    if (status.root === '') {
      this.status.textContent =
        'No folder indexed yet. Start Sonora with --library "C:\\Music" to index one.';
    } else if (status.scanning) {
      // filesRead against filesSeen is the interesting pair: on a rescan it stays
      // near zero, which is the incremental scan doing its job in public.
      this.status.textContent =
        `Scanning ${status.root} \u2014 ${status.filesSeen} file(s) seen, ` +
        `${status.filesRead} read, +${status.added} \u2022 ~${status.updated} \u2022 -${status.removed}` +
        (status.failed > 0 ? ` \u2022 ${status.failed} unreadable` : '');
    } else {
      // The last scan's numbers are part of the line, so that pressing Rescan
      // says something even when the answer is "nothing changed" -- and so that
      // "read 0 of 3 files" is visible, which is the whole point of an
      // incremental scan.
      const scan =
        status.filesSeen > 0
          ? ` \u00b7 last scan: ${status.filesSeen} seen, ${status.filesRead} read` +
            (status.added + status.updated + status.removed > 0
              ? `, +${status.added} ~${status.updated} -${status.removed}`
              : ', nothing changed')
          : '';
      this.status.textContent =
        `${status.trackCount} track(s), ${status.albumCount} album(s), ` +
        `${status.artistCount} artist(s) \u2014 ${status.root}${scan}` +
        (status.lastError === '' ? '' : ` \u2014 ${status.lastError}`);
    }

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

  private build(): void {
    this.input.type = 'search';
    this.input.placeholder = 'Search titles, artists, albums';
    this.input.setAttribute('aria-label', 'Search the library');
    this.input.addEventListener('input', () => {
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

    const rescan = element('button', 'library-rescan', 'Rescan');
    rescan.type = 'button';
    rescan.title = 'Look for files that changed since the last scan';
    rescan.addEventListener('click', () => {
      // Answered immediately, before any event arrives. A scan of an unchanged
      // folder is over in milliseconds and leaves the status text identical to
      // what it already said, so without this the button looks broken -- which
      // is what it looked like.
      this.status.textContent = 'Scanning…';
      void sonora.library
        .scan({})
        .then((result) => {
          if (!result.started) {
            this.status.textContent = 'A scan is already running.';
          }
        })
        .catch((error: unknown) => {
          this.status.textContent = describe(error);
        });
    });

    this.roots.search.replaceChildren(this.input, rescan);
    this.roots.sidebar.replaceChildren(this.navigation, this.status);
    this.updateNavigation();

    this.tracks = new VirtualList<LibraryTrack>(this.list, {
      rowHeight: 34,
      renderRow: (track, index) => this.trackRow(track, index),
      renderPending: () => element('div', 'track-row track-row-pending', '\u2026'),
    });

    this.roots.content.replaceChildren(this.heading, this.subheading, this.message, this.grid,
                                       this.list);
  }

  private updateNavigation(): void {
    const status = this.lastStatus;
    const entries: ReadonlyArray<{ label: string; view: View; kind: View['kind'] }> = [
      { label: `All tracks${status ? ` (${status.trackCount})` : ''}`, view: { kind: 'tracks' }, kind: 'tracks' },
      { label: `Albums${status ? ` (${status.albumCount})` : ''}`, view: { kind: 'albums' }, kind: 'albums' },
      { label: `Artists${status ? ` (${status.artistCount})` : ''}`, view: { kind: 'artists' }, kind: 'artists' },
    ];

    this.navigation.replaceChildren(
      ...entries.map(({ label, view, kind }) => {
        const button = element('button', 'library-nav-item', label);
        button.type = 'button';
        // An album or artist page is a place inside its section, so the section
        // stays lit while you are in one.
        const active =
          this.view.kind === kind ||
          (kind === 'albums' && this.view.kind === 'album') ||
          (kind === 'artists' && this.view.kind === 'artist');
        if (active) {
          button.dataset['active'] = 'true';
        }
        button.addEventListener('click', () => void this.show(view));
        return button;
      }),
    );
  }

  private async show(view: View): Promise<void> {
    this.view = view;
    this.updateNavigation();
    this.message.textContent = '';

    try {
      switch (view.kind) {
        case 'tracks': {
          const source = new PagedTracks(() => this.tracks?.invalidate());
          await source.first();
          this.showList(
            'All tracks',
            `${source.length} track(s), in album order`,
            source,
          );
          // The ids for "play from here" are only known page by page. Clicking a
          // row in this view queues that track and everything after it that has
          // arrived, which is what a listener means by it.
          this.listed = [];
          break;
        }
        case 'albums': {
          const result = await sonora.library.listAlbums();
          this.showAlbums(result.albums);
          break;
        }
        case 'artists': {
          const result = await sonora.library.listArtists();
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
            `${view.album.albumArtist}${view.album.year > 0 ? ` \u2022 ${view.album.year}` : ''} \u2022 ` +
              `${result.tracks.length} track(s) \u2022 ${formatDuration(view.album.durationMs)}`,
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
            `${result.tracks.length} track(s)`,
            arraySource(result.tracks),
            result.tracks,
          );
          break;
        }
        case 'search': {
          const result = await sonora.library.search({ query: view.query });
          this.showList(
            `Search: ${view.query}`,
            result.tracks.length === 0
              ? 'Nothing matched'
              : `${result.tracks.length} match(es), best first`,
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

  private showList(
    heading: string,
    subheading: string,
    source: RowSource<LibraryTrack>,
    tracks?: readonly LibraryTrack[],
    album?: LibraryAlbum,
  ): void {
    this.heading.replaceChildren(
      ...(album !== undefined ? [coverElement(album.artUrl, album.album, 'cover cover-small')] : []),
      document.createTextNode(heading),
    );
    this.subheading.textContent = subheading;
    this.listed = tracks?.map((track) => track.id) ?? [];

    if (tracks !== undefined && tracks.length > 0) {
      const play = element('button', 'library-play', 'Play all');
      play.type = 'button';
      play.addEventListener('click', () => void this.actions.play(this.listed));
      const add = element('button', 'library-add', 'Queue all');
      add.type = 'button';
      add.addEventListener('click', () => void this.actions.enqueue(this.listed));
      this.subheading.append(' ', play, add);
    }

    this.grid.replaceChildren();
    this.grid.hidden = true;
    this.list.hidden = false;
    this.tracks?.setSource(source);
  }

  private trackRow(track: LibraryTrack, index: number): HTMLElement {
    const row = element('div', 'track-row');
    row.dataset['id'] = String(track.id);

    const number = track.trackNumber > 0 ? String(track.trackNumber) : String(index + 1);
    row.append(
      element('span', 'track-number', number),
      element('span', 'track-title', track.title),
      element('span', 'track-artist', track.artist),
      element('span', 'track-album', track.album),
      element('span', 'track-time', formatTime(track.durationMs)),
    );

    const add = element('button', 'track-add', '+');
    add.type = 'button';
    add.title = 'Add to the queue';
    add.addEventListener('click', (event) => {
      // Or the click would also count as a click on the row.
      event.stopPropagation();
      void this.actions.enqueue([track.id]);
    });
    row.append(add);

    row.addEventListener('dblclick', () => {
      // This track, then the rest of what is on screen after it -- which is what
      // clicking a track in a list means to a listener. In the paged view the
      // rest is not known yet, so it is this track alone.
      const from = this.listed.indexOf(track.id);
      void this.actions.play(from >= 0 ? this.listed.slice(from) : [track.id]);
    });
    return row;
  }

  private showAlbums(albums: readonly LibraryAlbum[]): void {
    this.heading.replaceChildren(document.createTextNode('Albums'));
    this.subheading.textContent = `${albums.length} album(s)`;
    this.list.hidden = true;
    this.grid.hidden = false;
    this.listed = [];

    if (albums.length === 0) {
      this.message.textContent = 'Nothing indexed yet.';
    }

    // Not virtualized: an album grid is hundreds of cards, not tens of
    // thousands, and each one is a lazily loaded image the browser skips until it
    // is near the viewport. The track list is where the row count gets serious.
    this.grid.replaceChildren(
      ...albums.map((album) => {
        const card = element('button', 'album-card');
        card.type = 'button';
        card.append(
          coverElement(album.artUrl, album.album || album.albumArtist),
          element('span', 'album-name', album.album === '' ? 'Unknown album' : album.album),
          element('span', 'album-artist', album.albumArtist),
          element(
            'span',
            'album-meta',
            `${album.trackCount} track(s)${album.year > 0 ? ` \u2022 ${album.year}` : ''}`,
          ),
        );
        card.addEventListener('click', () => void this.show({ kind: 'album', album }));
        return card;
      }),
    );
  }

  private showArtists(artists: readonly string[]): void {
    this.heading.replaceChildren(document.createTextNode('Artists'));
    this.subheading.textContent = `${artists.length} artist(s)`;
    this.list.hidden = true;
    this.grid.hidden = false;
    this.listed = [];

    this.grid.replaceChildren(
      ...artists.map((artist) => {
        const card = element('button', 'artist-card', artist);
        card.type = 'button';
        card.addEventListener('click', () => void this.show({ kind: 'artist', artist }));
        return card;
      }),
    );
  }
}
