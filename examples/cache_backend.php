<?php
declare(strict_types=1);

use Bacnet\CacheBackendInterface;
use Bacnet\Client;

/** Kleine Demo; für mehrere Prozesse ist stattdessen etwa Redis geeignet. */
final class ArrayCacheBackend implements CacheBackendInterface
{
    private array $values = [];
    private array $generations = [];

    private function id(string $namespace, string $partition, string $key): string
    {
        return $namespace . '|' . $partition . '|' . $key;
    }

    public function get(string $namespace, string $partition, string $key): ?string
    {
        $id = $this->id($namespace, $partition, $key);
        $entry = $this->values[$id] ?? null;
        if (!$entry || $entry['expires'] <= (int) floor(microtime(true) * 1000)) {
            unset($this->values[$id]);
            return null;
        }
        return $entry['payload'];
    }

    public function set(string $namespace, string $partition, string $key, string $payload, int $expiresAtMs, int $maxEntries): void
    {
        $this->values[$this->id($namespace, $partition, $key)] = ['payload' => $payload, 'expires' => $expiresAtMs];
    }

    public function invalidate(string $namespace, string $partition, string $scope): void
    {
        $this->clear($namespace, $partition);
    }

    public function clear(string $namespace, ?string $partition): void
    {
        $prefix = $namespace . '|' . ($partition === null ? '' : $partition . '|');
        foreach (array_keys($this->values) as $key) if (str_starts_with($key, $prefix)) unset($this->values[$key]);
    }

    public function getGeneration(string $namespace, string $partition): int
    {
        return $this->generations[$namespace . '|' . $partition] ?? 0;
    }

    public function bumpGeneration(string $namespace, string $partition): int
    {
        return ++$this->generations[$namespace . '|' . $partition];
    }
}

$client = new Client(getenv('BACNET_DEMO_INTERFACE') ?: null);
$client->setCacheBackend(new ArrayCacheBackend());
print_r($client->getCacheOptions());
print_r($client->getCacheStats());
