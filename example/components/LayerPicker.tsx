import React, { memo, useCallback } from 'react';
import { StyleSheet, Text, TouchableOpacity, View } from 'react-native';

export type LayerOption = { label: string; assetId: number };

export const LAYER_OPTIONS: LayerOption[] = [
  { label: 'Terrain', assetId: 1 },
  { label: 'Bing', assetId: 2 },
  { label: 'Google', assetId: 3830182 },
  { label: 'Contour', assetId: 3830186 },
];

type LayerPillProps = {
  option: LayerOption;
  active: boolean;
  onSelect: (assetId: number) => void;
};

const LayerPill = memo(function LayerPill({
  option,
  active,
  onSelect,
}: LayerPillProps) {
  const handlePress = useCallback(
    () => onSelect(option.assetId),
    [onSelect, option.assetId],
  );

  return (
    <TouchableOpacity
      style={[styles.pill, active && styles.pillActive]}
      onPress={handlePress}
      activeOpacity={0.75}
    >
      <Text style={[styles.label, active && styles.labelActive]}>
        {option.label}
      </Text>
    </TouchableOpacity>
  );
});

export type LayerPickerProps = {
  selected: number;
  onSelect: (assetId: number) => void;
};

export function LayerPicker({ selected, onSelect }: LayerPickerProps) {
  return (
    <View style={styles.row}>
      {LAYER_OPTIONS.map((opt) => (
        <LayerPill
          key={opt.assetId}
          option={opt}
          active={opt.assetId === selected}
          onSelect={onSelect}
        />
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    backgroundColor: 'rgba(0,0,0,0.55)',
    borderRadius: 24,
    padding: 4,
    gap: 4,
  },
  pill: { paddingHorizontal: 12, paddingVertical: 7, borderRadius: 20 },
  pillActive: { backgroundColor: 'rgba(255,255,255,0.92)' },
  label: { fontSize: 13, fontWeight: '500', color: 'rgba(255,255,255,0.85)' },
  labelActive: { color: '#1a1a1a' },
});
