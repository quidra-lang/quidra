// Ambient declarations for the Node.js built-in modules used by this probe.
// Written in first-party TypeScript because the @types/node package cannot be
// installed on this host (global_rules.host_verification.no_installation_of_extra_software).
declare module "node:test" {
  function test(name: string, fn: () => void | Promise<void>): void;
  export default test;
}
declare module "node:assert" {
  const assert: {
    ok(value: unknown, message?: string): void;
    strictEqual(actual: unknown, expected: unknown, message?: string): void;
  };
  export default assert;
}
