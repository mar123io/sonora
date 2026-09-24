import {
  type Capability,
  DECLARED_CAPABILITIES,
  type DeclaredCapability,
  PROTOCOL_VERSION,
  type ShellGetCapabilitiesResult,
} from './generated';

// What the shell on the other side can actually do.
//
// The web layer and the native layer ship separately -- that is the point of
// having a bridge at all -- so the page is regularly newer than the shell
// hosting it, or older. Asking first and branching on the answer is what makes
// that survivable. The alternative is finding out by calling a method that is
// not there, at the moment a user clicks something.

export class CapabilitySet {
  private constructor(
    private readonly byName: ReadonlyMap<string, Capability>,
    /** The envelope version the shell speaks. */
    readonly shellProtocolVersion: number,
  ) {}

  static from(result: ShellGetCapabilitiesResult): CapabilitySet {
    const byName = new Map<string, Capability>();
    for (const capability of result.capabilities) {
      byName.set(capability.name, capability);
    }
    return new CapabilitySet(byName, result.protocolVersion);
  }

  /** Nothing is available. Used when the shell cannot even be asked. */
  static empty(): CapabilitySet {
    return new CapabilitySet(new Map(), 0);
  }

  /**
   * True when the capability is present, switched on, and at least
   * `minVersion`. A disabled capability and a missing one answer the same
   * here on purpose: from the page's point of view there is nothing to call
   * either way, and every caller that tried to tell them apart would get the
   * branch subtly wrong.
   */
  has(name: string, minVersion = 1): boolean {
    const capability = this.byName.get(name);
    return capability !== undefined && capability.enabled && capability.version >= minVersion;
  }

  /** Present but switched off in this run -- worth saying, as it is temporary. */
  isDisabled(name: string): boolean {
    const capability = this.byName.get(name);
    return capability !== undefined && !capability.enabled;
  }

  version(name: string): number | undefined {
    return this.byName.get(name)?.version;
  }

  get all(): readonly Capability[] {
    return [...this.byName.values()];
  }

  /**
   * Capabilities this bundle was built against that the shell does not offer
   * at the version it needs. Empty is the normal case; anything in it is a
   * feature the page has to do without.
   */
  get unmet(): readonly DeclaredCapability[] {
    return DECLARED_CAPABILITIES.filter((declared) => !this.has(declared.name, declared.version));
  }

  /**
   * True when the shell speaks an envelope version this bundle does not. The
   * check is exact rather than "at least": a higher number means the shell may
   * frame things in a way this parser has never seen, and a lower one means it
   * cannot read what this page sends.
   */
  get protocolMatches(): boolean {
    return this.shellProtocolVersion === PROTOCOL_VERSION;
  }
}
