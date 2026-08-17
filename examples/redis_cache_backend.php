<?php
declare(strict_types=1);

use Bacnet\CacheBackendInterface;

/** Redis ist nur ein möglicher Adapter; die Extension hängt nicht von ext-redis ab. */
final class RedisCacheBackend implements CacheBackendInterface
{
    public function __construct(private Redis $redis, private string $prefix = 'bacnet:') {}

    private function key(string $namespace, string $partition, string $key): string
    {
        return $this->prefix . hash('sha256', $namespace . "\0" . $partition . "\0" . $key);
    }

    public function get(string $namespace, string $partition, string $key): ?string
    {
        $value = $this->redis->get($this->key($namespace, $partition, $key));
        return $value === false ? null : $value;
    }

    public function set(string $namespace, string $partition, string $key, string $payload, int $expiresAtMs, int $maxEntries): void
    {
        $ttl = max(1, $expiresAtMs - (int) floor(microtime(true) * 1000));
        $redisKey = $this->key($namespace, $partition, $key);
        $this->redis->pSetEx($redisKey, $ttl, $payload);
        $this->redis->sAdd($this->prefix . 'index:' . hash('sha256', $namespace . "\0" . $partition), $redisKey);
    }

    public function invalidate(string $namespace, string $partition, string $scope): void
    {
        $this->clear($namespace, $partition);
        $this->bumpGeneration($namespace, $partition);
    }

    public function clear(string $namespace, ?string $partition): void
    {
        $partitions = $partition === null ? ['state', 'object', 'object_list', 'device', 'ip', 'negative'] : [$partition];
        foreach ($partitions as $name) {
            $index = $this->prefix . 'index:' . hash('sha256', $namespace . "\0" . $name);
            $keys = $this->redis->sMembers($index);
            if ($keys) $this->redis->del($keys);
            $this->redis->del($index);
        }
    }

    public function getGeneration(string $namespace, string $partition): int
    {
        return (int) ($this->redis->get($this->generationKey($namespace, $partition)) ?: 0);
    }

    public function bumpGeneration(string $namespace, string $partition): int
    {
        return $this->redis->incr($this->generationKey($namespace, $partition));
    }

    private function generationKey(string $namespace, string $partition): string
    {
        return $this->prefix . 'generation:' . hash('sha256', $namespace . "\0" . $partition);
    }
}

$redis = new Redis();
$redis->connect(getenv('REDIS_HOST') ?: '127.0.0.1', (int) (getenv('REDIS_PORT') ?: 6379));
$client = new Bacnet\Client(getenv('BACNET_DEMO_INTERFACE') ?: null);
$client->setCacheBackend(new RedisCacheBackend($redis));
print_r($client->getCacheOptions());
