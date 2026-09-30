# Purpose: Resolve a credential-free GitHub repository consistently for audit and workflow tools.
# Inputs: Optional owner/repo override and the absolute repository checkout root.
# Outputs: Returns owner/repo or throws without exposing rejected remote text.
function Resolve-GitHubRepository {
    param([string]$Repository, [Parameter(Mandatory = $true)][string]$RepositoryRoot)

    if (-not [string]::IsNullOrWhiteSpace($Repository)) {
        if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
            throw 'Repository must use owner/repo form.'
        }
        return $Repository
    }

    $remote = (git -C $RepositoryRoot config --get remote.origin.url)
    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot resolve GitHub repository because reading remote.origin.url failed.'
    }
    if ([string]::IsNullOrWhiteSpace($remote)) {
        throw 'Cannot resolve GitHub repository because remote.origin.url is unset.'
    }
    if ($remote -is [string] -and
        $remote -match '^(https://github\.com/|ssh://git@github\.com/|git@github\.com:)(?<slug>[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(\.git)?$') {
        return $Matches.slug
    }
    throw 'Cannot parse GitHub repository from remote.origin.url; use a credential-free GitHub remote or explicit owner/repo.'
}
