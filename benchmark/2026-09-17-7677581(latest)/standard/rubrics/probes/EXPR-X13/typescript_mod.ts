function privFn(): number { return 7; }          // non-exported: module-private
export function pubFn(): number { return privFn(); }   // exported: public
