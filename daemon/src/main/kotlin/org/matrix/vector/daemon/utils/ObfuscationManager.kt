package org.matrix.vector.daemon.utils

import android.os.SharedMemory

object ObfuscationManager {
  /**
   * Rewrites the package/class names (and, when [renameMembers] is set, the member names) that
   * appear in [memory] and returns a shared memory holding the result.
   *
   * The flag is passed in rather than read here because the data directory is a constant of the
   * `FileSystem` object; duplicating that path in native code would be a second place to keep in
   * sync. Whichever of the two entry points is reached first fixes the decision for the process.
   */
  @JvmStatic external fun obfuscateDex(memory: SharedMemory, renameMembers: Boolean): SharedMemory

  /** The table the injected processes resolve names through; see [obfuscateDex]. */
  @JvmStatic external fun getSignatures(renameMembers: Boolean): Map<String, String>
}
