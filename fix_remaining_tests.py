#!/usr/bin/env python3

import re
from pathlib import Path

def fix_file(filepath: Path) -> int:
    """Fix various hash_options issues in a single file."""
    
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return 0
    
    original_content = content
    replacements = 0
    
    # Fix duplicate keylet calls with hash_options
    # Pattern: keylet::X(keylet::X(hash_options{...}, ...))
    pattern1 = re.compile(
        r'keylet::(\w+)\s*\(\s*keylet::\1\s*\(\s*hash_options\s*\{[^}]+\}[^)]*\)\s*\)',
        re.MULTILINE
    )
    
    def fix_duplicate(match):
        nonlocal replacements
        # Extract just the inner keylet call
        inner = match.group(0)
        # Find the position of the second keylet::
        second_keylet_pos = inner.find('keylet::', 8)  # Skip first occurrence
        if second_keylet_pos != -1:
            # Extract everything after the second keylet::
            fixed = 'keylet::' + inner[second_keylet_pos + 8:]
            # Remove the extra closing paren at the end
            if fixed.endswith('))'):
                fixed = fixed[:-1]
            replacements += 1
            return fixed
        return match.group(0)
    
    content = pattern1.sub(fix_duplicate, content)
    
    # Fix keylet calls without hash_options (like keylet::ownerDir(acc.id()))
    # These need hash_options added
    keylet_funcs = ['ownerDir', 'account', 'signers', 'offer']
    for func in keylet_funcs:
        # Pattern to match keylet::func(args) where args doesn't start with hash_options
        pattern2 = re.compile(
            rf'keylet::{func}\s*\(\s*(?!hash_options)([^)]+)\)',
            re.MULTILINE
        )
        
        def add_hash_options(match):
            nonlocal replacements
            args = match.group(1).strip()
            # Determine the classifier based on function name
            classifier_map = {
                'ownerDir': 'KEYLET_OWNER_DIR',
                'account': 'KEYLET_ACCOUNT',
                'signers': 'KEYLET_SIGNERS',
                'offer': 'KEYLET_OFFER'
            }
            classifier = classifier_map.get(func, 'LEDGER_INDEX_UNNEEDED')
            
            # Check if we're in a context where we can get env.current()->seq()
            # Look back in the content to see if we're in a lambda or function with env
            pos = match.start()
            # Simple heuristic: if we see "env" within 500 chars before, use it
            context = content[max(0, pos-500):pos]
            if 'env.' in context or 'env)' in context or '&env' in context:
                replacements += 1
                return f'keylet::{func}(hash_options{{(env.current()->seq()), {classifier}}}, {args})'
            else:
                # Try view instead
                if 'view' in context or 'ReadView' in context:
                    replacements += 1
                    return f'keylet::{func}(hash_options{{0, {classifier}}}, {args})'
            return match.group(0)
        
        content = pattern2.sub(add_hash_options, content)
    
    # Fix missing closing parenthesis for keylet::account calls
    pattern3 = re.compile(
        r'(keylet::account\s*\(\s*hash_options\s*\{[^}]+\}\s*,\s*\w+(?:\.\w+\(\))?\s*)(\);)',
        re.MULTILINE
    )
    
    def fix_paren(match):
        nonlocal replacements
        replacements += 1
        return match.group(1) + '));'
    
    content = pattern3.sub(fix_paren, content)
    
    if replacements > 0 and content != original_content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        return replacements
    
    return 0

def main():
    project_root = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc")
    
    # Files to fix
    test_files = [
        "src/test/app/RCLValidations_test.cpp",
        "src/test/app/PayStrand_test.cpp", 
        "src/test/app/PayChan_test.cpp",
        "src/test/app/ClaimReward_test.cpp",
        "src/test/app/Import_test.cpp",
        "src/test/app/LedgerReplay_test.cpp",
        "src/test/app/Offer_test.cpp"
    ]
    
    total_replacements = 0
    
    for rel_path in test_files:
        filepath = project_root / rel_path
        if filepath.exists():
            replacements = fix_file(filepath)
            if replacements > 0:
                print(f"Fixed {rel_path}: {replacements} replacements")
                total_replacements += replacements
    
    print(f"\nTotal replacements: {total_replacements}")

if __name__ == "__main__":
    main()