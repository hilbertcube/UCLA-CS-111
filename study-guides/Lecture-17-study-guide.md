# Lecture 17 — RAID and OS Security

---

## Overview

Two major topics, both about making systems survive hostile conditions:

1. **RAID** — how to survive individual disk failures using redundancy; RAID 0 through RAID 5; the math of parity; reliability statistics
2. **OS Security** — threat models, authentication, authorization (Unix permissions vs. ACLs vs. capabilities)

---

## Part 1: RAID — Redundant Array of Independent Disks

**The problem RAID solves:** Individual disks fail. A single 1 TB disk drive that holds all your data has a mean time to failure (MTTF) of ~300,000 hours (~34 years). Sounds long, but a data center with 10,000 drives expects a failure roughly **every 30 hours**. RAID uses multiple drives together to provide failure tolerance, higher performance, or both.

**Key insight:** RAID is implemented at the **block device level** — above the hardware, below the file system. The file system sees a single virtual disk; RAID manages the distribution across physical drives.

> **"RAID nests":** The drives in a RAID array do not have to be physical disks — they can themselves be RAID arrays or other virtual block devices. RAID can be stacked.

---

### RAID 0 — Striping (No Redundancy)

**Goal:** Performance. Use multiple drives as one large, fast drive.

**Two approaches:**

**Approach 1 — Concatenation:**
```
Drive 1: blocks  0 – 999
Drive 2: blocks 1000 – 1999
Drive 3: blocks 2000 – 2999
```
Blocks 0–999 live on drive 1, 1000–1999 on drive 2, etc. The OS sees one large drive. Reads and writes to different drive regions can proceed in parallel, but a sequential workload hits only one drive at a time.

**Approach 2 — Striping (true RAID 0):**
```
Drive 1: blocks 0, 3, 6, 9, ...   (every 3rd block)
Drive 2: blocks 1, 4, 7, 10, ...
Drive 3: blocks 2, 5, 8, 11, ...
```
Consecutive logical blocks are spread ("striped") across all drives. A sequential read now reads from all drives simultaneously → **parallel I/O, maximum throughput**.

**RAID 0 properties:**
- ✅ Read and write throughput scales with number of drives
- ✅ Total capacity = sum of all drive capacities
- ❌ **Zero redundancy** — if any single drive fails, the entire array is lost
- ❌ Actually increases failure probability compared to a single drive (more drives = more chances of failure)

> **Use case:** Scratch storage, temporary data, or workloads where performance matters more than durability (video editing scratch disks, game load caches).

---

### RAID 1 — Mirroring

**Goal:** Survive one drive failure with the simplest possible approach.

**Mechanism:** Keep two exact copies of every block on two separate drives.

```
Drive 1 (primary):  blocks 0, 1, 2, 3, ...
Drive 2 (mirror):   blocks 0, 1, 2, 3, ...   ← identical copy
```

**Read performance:** Improved — the controller can choose whichever drive has its head closest to the requested sector, reducing seek time. With two drives, you can also serve two concurrent reads.

**Write performance:** Degraded — every write must go to both drives (sequentially or in parallel, but both must complete). Write throughput is approximately the same as a single drive.

**Storage efficiency:** 50% — you pay for 2 TB of drives but get 1 TB of usable space.

**RAID 1 properties:**
- ✅ Survives any single drive failure
- ✅ Read performance improvement
- ❌ Write performance same as one drive
- ❌ 50% storage efficiency (most expensive per usable byte)

---

### RAID 4 — Dedicated Parity Drive

**Goal:** Survive one drive failure at lower cost than mirroring.

**Mechanism:** Use N−1 data drives and 1 dedicated parity drive. The parity drive stores the XOR of all corresponding data blocks.

```
Drive 1 (data):    A
Drive 2 (data):    B
Drive 3 (data):    C
Drive 4 (data):    D
Drive 5 (parity):  P = A ⊕ B ⊕ C ⊕ D
```

**Why XOR?** XOR is its own inverse: `X ⊕ X = 0` and `X ⊕ 0 = X`. Any single unknown in a set of XOR'd values can be recovered by XOR-ing all the known values and the parity together.

#### Recovery from Data Drive Failure

If drive 2 (B) fails:

```
B = P ⊕ A ⊕ C ⊕ D
  = (A ⊕ B ⊕ C ⊕ D) ⊕ A ⊕ C ⊕ D
  = B   ✅  (all other terms cancel)
```

In words: XOR the parity with all surviving data drives → the result is the missing drive's data.

#### Recovery from Parity Drive Failure

Even easier: recompute parity from the surviving data drives: `P = A ⊕ B ⊕ C ⊕ D`.

#### Updating Parity on a Write

When data block B changes to B′, the parity must be updated. **Naive approach:** re-read A, C, D and recompute P = A ⊕ B′ ⊕ C ⊕ D — requires reading 3 drives.

**Efficient approach — "read-modify-write":**
```
P_new = P_old ⊕ B_old ⊕ B_new
```
This requires only 2 reads (P_old and B_old) and 2 writes (B_new and P_new). Proof:
```
P_new = A ⊕ B_new ⊕ C ⊕ D
      = A ⊕ B_new ⊕ C ⊕ D ⊕ (B_old ⊕ B_old)    [insert B_old ⊕ B_old = 0]
      = (A ⊕ B_old ⊕ C ⊕ D) ⊕ B_old ⊕ B_new
      = P_old ⊕ B_old ⊕ B_new   ✅
```

#### RAID 4 Limitations

**The parity drive is a write bottleneck:** Every single write to any data drive also requires a write to the parity drive. Even if you're only writing to drive 1, the parity drive must be updated. With many concurrent writes, all of them serialize on the single parity drive.

**RAID 4 properties:**
- ✅ Survives any single drive failure
- ✅ Storage efficiency: (N−1)/N — much better than mirroring for large N
- ❌ Parity drive is a write bottleneck
- ❌ Cannot survive 2+ simultaneous drive failures

---

### RAID 5 — Distributed Parity

**Goal:** RAID 4 fault tolerance without the parity bottleneck.

**Mechanism:** Distribute parity blocks across all drives in a rotating pattern. No single drive is dedicated to parity.

```
Block offset:   Drive 1    Drive 2    Drive 3    Drive 4
    0:            A0          B0          C0         P0  ← P is on drive 4
    1:            A1          B1          P1         D1  ← P is on drive 3
    2:            A2          P2          C2         D2  ← P is on drive 2
    3:            P3          B3          C3         D3  ← P is on drive 1
```

**Why this fixes the bottleneck:** Writes to different logical blocks update parity on different physical drives. Concurrent writes can proceed in parallel across multiple drives instead of all serializing on one.

**RAID 5 vs RAID 4:**

| Property | RAID 4 | RAID 5 |
|----------|--------|--------|
| Parity location | Dedicated drive | Distributed across all drives |
| Write bottleneck | Yes — all writes hit parity drive | No — writes spread across drives |
| Read performance | Similar to RAID 4 | Similar |
| Fault tolerance | Any 1 drive | Any 1 drive |
| Storage efficiency | (N−1)/N | (N−1)/N (same) |
| Implementation complexity | Simpler | More complex |

> **Exam trap:** RAID 5 does NOT improve fault tolerance over RAID 4 (both tolerate exactly 1 failure). The improvement is **write performance** (no parity bottleneck). Adding a second failure kills both.

---

### RAID 6 — Two Parity Blocks

**Extension of RAID 5:** Uses two independent parity calculations per stripe, distributed across all drives.

- Survives **any 2 simultaneous drive failures**
- Storage efficiency: (N−2)/N
- Write overhead: 2 parity updates per write
- Used in high-availability production systems (cloud storage, enterprise NAS)

---

### Reliability Statistics: MTTF, MTBF, Availability

Understanding how reliable a storage system is requires statistical modeling:

| Metric | Definition |
|--------|-----------|
| **MTTF** (Mean Time To Failure) | Average time a component operates before failing |
| **MTTR** (Mean Time To Repair) | Average time to detect, procure, and replace a failed drive |
| **MTBF** (Mean Time Between Failures) | Average time between the start of one failure and the start of the next: MTBF = MTTF + MTTR |

```
Timeline:
|──── MTTF ────|─MTTR─|──── MTTF ────|─MTTR─|
       up        down        up         down
|──────────── MTBF ───────────|
```

**Availability formula:**
```
Availability = MTTF / MTBF = MTTF / (MTTF + MTTR)
Downtime fraction = 1 - Availability = MTTR / (MTTF + MTTR)
```

**Typical values:**
| Device | MTTF | Equivalent |
|--------|------|-----------|
| HDD | ~300,000 hours | ~34 years |
| Flash/SSD | ~2,000,000 hours | ~228 years |

> **Important caveat:** MTTF is a statistical average across many devices, not a guarantee for any individual drive. A drive with 300,000-hour MTTF can fail on day 1 — it just means the *average* across a large population is 34 years.

### The Bathtub Curve

The **failure rate** of storage devices follows a bathtub-shaped curve over time:

```
Failure
 rate
  │\                                /
  │  \                             /
  │    \___________________________/
  │    infant   useful life    wear-out
  └──────────────────────────────→ time
```

| Phase | Cause | Duration |
|-------|-------|---------|
| **Infant mortality** | Manufacturing defects; early component failures | Days to months |
| **Useful life** | Random independent failures (rare) | Years |
| **Wear-out** | Physical degradation; HDD bearings; flash P/E exhaustion | Toward end of rated life |

> **Implication:** Burn-in testing (running new drives under load for 24–72 hours) eliminates infant mortality failures before deployment. Enterprise drives are typically burn-in tested.

---

### RAID Failure Scenarios

| Scenario | RAID 1 | RAID 4/5 | RAID 6 |
|----------|--------|---------|--------|
| 0 failures | ✅ | ✅ | ✅ |
| 1 failure | ✅ recover from mirror | ✅ recover via XOR | ✅ |
| 2 simultaneous failures | ❌ depends on which two | ❌ | ✅ |
| Parity drive fails (RAID 4) | N/A | ✅ (recompute parity) | ✅ |

> **"If one drive fails, replace it quickly."** While operating in degraded mode (1 failure in RAID 5), the array has zero fault tolerance. A second failure causes complete data loss. The window between detecting a failure and completing a rebuild (hours to days for large drives) is dangerously exposed.

---

## Part 2: OS Security

### Threat Model: What Are We Defending Against?

Security defenses come in two forms:
- **Force:** Physical barriers, hardware locks, tamper-evident seals — prevent physical access
- **Fraud:** Software and protocol mechanisms — prevent unauthorized logical access

This lecture focuses on fraud-based attacks.

### Types of Fraud Attacks

| Attack type | Description |
|-------------|-------------|
| **Privacy attack** | Unauthorized release of information — reading data you shouldn't see (eavesdropping, data exfiltration) |
| **Integrity attack** | Unauthorized modification of data — changing data, injecting code, corrupting records |
| **Availability attack (DoS)** | Denial of Service — making the system unusable for legitimate users |

### Threat Classification

| Threat source | Description | Examples |
|--------------|-------------|---------|
| **Insiders** | Authorized users who misuse their access | Disgruntled employee stealing customer data |
| **Social engineering** | Attacker impersonates an insider to gain access | Phishing, pretexting, fake IT support calls |
| **Network attacks** | Attacker on the internet exploits software vulnerabilities | Viruses, drive-by downloads, DoS floods, buffer overruns |
| **Device attacks** | Malicious hardware plugged into a machine | Malicious USB ("Rubber Ducky"), compromised firmware |

**Buffer overrun (buffer overflow):** A classic network/local attack. If a program reads user input into a fixed-size stack buffer without checking length, an attacker can write past the end of the buffer and overwrite the saved return address on the stack, redirecting execution to attacker-controlled code. Defending against this requires: bounds checking, stack canaries, ASLR, non-executable stack (NX bit).

---

### The General Security Functions

Every secure system must implement these (and they must all work together):

| Function | Description | Example |
|----------|-------------|---------|
| **Authentication** | Verify that a principal is who they claim to be | Password check, biometric scan, SSH key |
| **Integrity** | Detect unauthorized modification of data | Checksums, HMACs, digital signatures |
| **Authorization** | Control which authenticated principals can perform which actions | Unix permissions, ACLs |
| **Auditing** | Record who did what and when, for forensic analysis | System logs, `/var/log/auth.log` |
| **Correctness** | The system does what it's supposed to do (and only that) | Formal verification, testing |

---

### Authentication: Proving Identity

There are three factors for authentication (multi-factor combines two or more):

| Factor | Description | Examples |
|--------|-------------|---------|
| **Something you are** | Biometrics unique to the person | Fingerprint, retina scan, face ID |
| **Something you have** | A physical token that proves identity | Hardware security key (YubiKey), smart card, phone (TOTP app) |
| **Something you know** | Secret known only to the principal | Password, PIN, security question |

**Why passwords must not be guessable (even by brute force):** An attacker with offline access to a password hash can try billions of combinations per second. Password requirements (length, entropy), salting, and key-stretching algorithms (bcrypt, Argon2) make brute force infeasible.

**Possible password attacks:**
- **Password guessing:** Try common passwords or dictionary words
- **Network snooping:** Intercept the password as it travels over the network → defense: encrypt the connection (TLS/SSH)
- **Credential stuffing:** Use leaked password databases from other breaches
- **Phishing:** Trick the user into entering their password on a fake site

### External vs. Internal Authentication

**External authentication:** Verifying a user's identity when they first connect to the system.

Example — SSH to SEASnet:
```
User → ssh sshd (on SEASnet)
         ↓
     sshd checks credentials:
       - password (plaintext is never sent; challenge-response over encrypted channel)
       - ssh public key (server checks if user's public key matches an authorized_keys file)
       - duo second factor
         ↓
     if accepted: creates authenticated session for that user
```

**Internal authentication:** After initial login, the OS must efficiently and unforgably track the user's identity for every subsequent operation.

- Identity is stored in the **process descriptor** (PCB) as UID/GID fields
- Every system call that touches a protected resource (file, socket, device) consults the UID/GID to check authorization
- The UID can only be changed by **trusted code** (the kernel, or setuid programs) — never by the user directly
- Identity is also recorded in audit logs and reported in `ps`, `who`, `last`, etc.

---

### Authorization: Who Can Do What

After proving who you are (authentication), the system must decide what you're allowed to do (authorization).

#### Unix/Linux Permission Bits (Simple Model)

Traditional Unix uses a simple three-group model:

```
-rwxr-x---  1  eggert  faculty  4096  Jun 1  file.txt
  │││ │││ │││
  │││ │││ └──── other:  no access (---)
  │││ └──────── group:  read + execute (r-x)
  └──────────── owner:  read + write + execute (rwx)
```

- **Owner (user):** The user who owns the file
- **Group:** Members of the owning group
- **Other:** Everyone else

Three permission bits per group: **r** (read), **w** (write), **x** (execute for files; list/traverse for directories).

**Limitation:** Only three tiers. You cannot say "Alice can read, Bob can write, Charlie can only execute." The permission model is too coarse for complex sharing requirements.

#### ACLs — Access Control Lists (Better Model)

An **Access Control List (ACL)** associates with each file a list of (principal, permissions) pairs:

```
file.txt ACL:
  alice:   rw-     (read and write)
  bob:     r--     (read only)
  charlie: --x     (execute only)
  @admins: rwx     (read, write, execute)
  other:   ---     (no access)
```

The owner of the file can freely modify the ACL. Linux supports POSIX ACLs, managed with:

```bash
setfacl -m u:alice:rw file.txt     # give alice read+write
getfacl file.txt                    # display the ACL
```

ACLs subsume the traditional Unix model: the owner/group/other bits are just the default ACL entries.

**ACL model:** Associated with the **object** (the file). The file carries the record of who can access it. To check if subject S can perform action A on object O, look up S in O's ACL.

---

#### Capabilities — The Alternative Model

A **capability** is a token that grants its holder specific access to a specific object. Unlike an ACL (stored with the object), a capability is held by the **subject** (the process).

```
Process P holds capabilities:
  cap1: read  → file.txt
  cap2: write → /tmp/scratch
  cap3: exec  → /usr/bin/ls
```

To access an object, the process presents the appropriate capability. The OS checks that the capability is valid and authorizes the access.

**Two implementations:**

| Approach | Description | Example |
|----------|-------------|---------|
| **OS-managed table** | Capabilities stored in kernel; process has an index (handle) into the kernel table | **File descriptors** — an fd is a capability: it grants the holding process specific access (read, write, or both) to a specific file. Every `syscall(fd, ...)` is checked against the kernel table. |
| **Cryptographic token** | Capability is a large encrypted/signed integer (e.g., 2048 bits). Anyone holding the token can use it; forgery is computationally infeasible. | Signed JWT tokens, macaroons, NFS file handles with signed components. Advantage: works across distributed systems without contacting a central authority. |

**ACLs vs. Capabilities:**

| Property | ACL | Capability |
|----------|-----|-----------|
| Where stored | With the object | With the subject (process) |
| Question answered | "Who can access this file?" | "What can this process access?" |
| Revocation | Easy — update the ACL | Hard — must find and invalidate all copies of the capability |
| Delegation | Harder | Easy — just give a copy of the capability to another process |
| Distributed use | Requires checking with server | Cryptographic capabilities work offline |

**Both must be:**
- **Unforgeable** — a process cannot create a fake capability or fake ACL entry
- **Consulted by the OS** before every access — security is enforced at the kernel boundary, not by trusting application code

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| RAID 0 striping | Spreads data across N drives; N× read throughput; no redundancy — any failure = total loss |
| RAID 1 mirroring | Full copy on 2 drives; survives 1 failure; 50% efficiency; better reads, same writes |
| RAID 4 | N−1 data + 1 parity (XOR); survives 1 failure; parity drive is write bottleneck |
| RAID 4 recovery | Lost block = XOR of all remaining blocks + parity |
| RAID 4 parity update | P_new = P_old ⊕ B_old ⊕ B_new (only 2 reads + 2 writes needed) |
| RAID 5 vs RAID 4 | Same fault tolerance; RAID 5 distributes parity — eliminates parity bottleneck |
| RAID 6 | Two independent parity blocks; survives 2 simultaneous failures |
| MTTF vs MTBF | MTTF = mean time to failure; MTBF = MTTF + MTTR; Availability = MTTF / MTBF |
| Bathtub curve | Three phases: infant mortality → useful life (low flat rate) → wear-out |
| Degraded mode risk | RAID 5 with 1 failed drive = 0 fault tolerance; replace failed drive fast |
| Fraud attack types | Privacy (unauthorized read), Integrity (unauthorized write), Availability (DoS) |
| Buffer overrun | Overflow stack buffer to overwrite return address; redirects execution to attacker code |
| Three auth factors | Something you are (biometrics), have (token), know (password) |
| Internal auth | UID/GID in process descriptor; only kernel can change it; checked on every syscall |
| Unix permissions | owner/group/other × rwx = 9 bits; too coarse for complex sharing |
| ACL | Per-object list of (principal, permissions); flexible; stored with the object |
| Capability | Per-process token granting access to specific object; file descriptors are capabilities |
| ACL vs capability | ACL: "who can access this?"; Capability: "what can this process access?" |
| Both requirements | Unforgeable + consulted by OS before every access |
| Crypto capabilities | Large signed integer; unforgeable; works across distributed systems without central server |
